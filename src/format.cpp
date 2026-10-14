
#include "format.h"
#include "altparse.h"
#include "treewalk.h"
#include "valueutil.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/print.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/source-path.hh>
#include <nix/util/util.hh>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

namespace gpin{

using std::string;
using std::string_view;
using nix::Value;
using nix::ExprAttrs;
using nix::Bindings;

struct IndentInfo{
	// A series of whitespace characters present at the start of every line.
	string_view preindent;
	// A series of whitespace characters representing one level of indentation.
	string_view indent;
	// Current indentation level.
	uint32_t level;
};

struct FormatState{
	RewriteLink* cursor;
	IndentInfo indentInfo;
public:
	FormatState(){}
	FormatState(std::unique_ptr<RewriteLink>& t){
		t.reset(cursor = new RewriteLink());
	}
	FormatState(FormatState&& that){
		cursor = that.cursor;
		indentInfo = that.indentInfo;
		that.cursor = nullptr;
	}
	FormatState(const FormatState&) = delete;
	FormatState& operator=(FormatState&& that){
		this->~FormatState();
		new (this) FormatState(std::move(that));
		return *this;
	}
	FormatState& operator=(const FormatState&) = delete;
private:
	RewriteLink& push(){
		RewriteLink* nl = new RewriteLink();
		nl->next = std::move(cursor->next);
		cursor->next.reset(nl);
		cursor = nl;
		return *nl;
	}
public:
	// Split the insertion cursor into two. Everything added to the new returned format state will precede anything subsequently added to this format state. The lifetimes of the states are not interrelated.
	FormatState split(){
		FormatState temp;
		temp.cursor = cursor;
		temp.indentInfo = indentInfo;
		push();
		return temp;
	}
	FormatState& operator<<(string str){
		if(str.empty()){ return *this; }
		if(cursor->view().empty()){
			cursor->data = std::move(str);
			return *this;
		}
		if(std::holds_alternative<string>(cursor->data)){
			std::string& s = std::get<string>(cursor->data);
			if(s.size() + str.size() <= s.capacity()){
				s += std::move(str);
				return *this;
			}
		}
		push().data = std::move(str);
		return *this;
	}
	FormatState& operator<<(string_view view){
		if(view.empty()){ return *this; }
		if(cursor->view().empty()){
			cursor->data = view;
			return *this;
		}
		if(view.size() < 0x10 && std::holds_alternative<string>(cursor->data)){
			std::string& s = std::get<string>(cursor->data);
			if(s.size() + view.size() <= s.capacity()){
				s += view;
				return *this;
			}
		}
		push().data = view;
		return *this;
	}
	template<size_t N>
	FormatState& operator<<(const char (&str)[N]){
		return this->operator<<(string_view(str));
	}
};

IndentInfo detectIndentation(string_view source,uint32_t position){
	string_view preindent = {};
	string_view lineAtPosition = {};
	std::unordered_map<string_view, uint32_t> frequencyMap;
	std::optional<string_view> prevLine;
	using pos = string_view::size_type;

	while(source.size() != 0){
		pos c = source.find_first_of('\n');
		string_view line1;
		if(c == string_view::npos){
			line1 = source;
			source = {};
		}else{
			line1 = source.substr(0, c);
			source = source.substr(c + 1);
		}
		if(position <= line1.size()){
			lineAtPosition = line1;
			position = 0xFFFFFFFF;
		}else if(position != 0xFFFFFFFF){
			position -= (line1.size() + 1);
		}
		pos x = line1.find_first_not_of("\t ");
		if(x != string_view::npos){
			line1 = line1.substr(0,x);
		}

		if(prevLine){
			string_view line0 = prevLine.value();

			uint32_t i = 0;
			while(i < line0.size() && i < line1.size() && line0[i] == line1[i]){
				i++;
			}

			frequencyMap.try_emplace(line0.substr(i),0).first->second++;
			frequencyMap.try_emplace(line1.substr(i),0).first->second++;
			
			uint32_t j = 0;
			while(j < line1.size() && j < preindent.size() && line1[j] == preindent[j]){
				j++;
			}
			preindent = preindent.substr(0, j);
		}else{
			preindent = line1;
		}

		prevLine.emplace(line1);
	}
	
	std::optional<std::pair<string_view, uint32_t>> bestPair;

	for(auto& pair : frequencyMap){
		if(!pair.first.empty() && (!bestPair || bestPair->second < pair.second)){
			bestPair = pair;
		}
	}
	if(!bestPair){
		return {};
	}

	uint32_t repeatCount = 0;

	if(lineAtPosition.starts_with(preindent)){
		lineAtPosition = lineAtPosition.substr(preindent.size());
		while(lineAtPosition.starts_with(bestPair->first)){
			repeatCount++;
			lineAtPosition = lineAtPosition.substr(bestPair->first.size());
		}
	}
	return {preindent, bestPair->first, repeatCount};
}

static IndentInfo detectIndentation(RewriteState& state, uint32_t begin, uint32_t end, uint32_t focus){
	using pos = string_view::size_type;
	pos lineStart = state.source.substr(0, begin).find_last_of('\n');
	lineStart = lineStart == string_view::npos ? 0 : lineStart + 1;
	return detectIndentation(state.source.substr(lineStart, end - lineStart), focus - lineStart);
}

struct chart_info{
	std::vector<nix::Symbol> path;
	uint32_t strength; // Already existing paths take priority over paths we make.
	inline chart_info(): path(), strength(0){}
};

static void chart(nix::Expr* expr, Value* value, std::vector<nix::Symbol>& path, std::unordered_map<Value*, chart_info>& paths){
	if(!path.empty()){
		auto& info = paths[value];
		int strength = expr == nullptr ? 1 : 2;
		if(info.strength < strength || info.strength == strength && path.size() < info.path.size()){
			info.path = path;
			info.strength = strength;
		}else{
			return;
		}
	}
	if(value->type() != nix::nAttrs){
		return;
	}
	const ExprAttrs* attrs = dynamic_cast<ExprAttrs*>(expr);
	for(auto& bnd : *value->attrs()){
		nix::Expr* subexpr = nullptr;
		if(attrs != nullptr){
			auto itr = attrs->attrs->find(bnd.name);
			if(itr != attrs->attrs->end()){
				subexpr = itr->second.e;
			}
		}
		path.push_back(bnd.name);
		chart(subexpr, bnd.value, path, paths);
		path.pop_back();
	}
}

static FormatState addRewrite(RewriteState& rs,uint32_t begin,uint32_t end){
#ifndef NDEBUG
	return FormatState(rs.replacement.insert({RewriteRange{begin, end, rs.offsetToNixPos(begin)}, nullptr})->second);
#else
	return FormatState(rs.replacement.insert({{begin, end}, nullptr})->second);
#endif
}

static FormatState addRewrite(RewriteState& rs,uint32_t begin){
	return addRewrite(rs, begin, begin);
}

static void eraseRange(RewriteState& rs,uint32_t begin,uint32_t end){
#ifndef NDEBUG
	rs.replacement.insert({RewriteRange{begin, end, rs.offsetToNixPos(begin)}, nullptr});
#else
	rs.replacement.insert({{begin, end}, nullptr});
#endif
}

static bool newLine(RewriteState& rs, FormatState& fs){
	if(fs.indentInfo.indent.size() == 0){
		return false; // No new lines.
	}
	fs << "\n" << fs.indentInfo.preindent;
	for(int32_t i = 0;i < fs.indentInfo.level;i++){
		fs << fs.indentInfo.indent;
	}
	return true;
}

static void addBrackets(RewriteState& rs, FormatState& state){
	state << "(";
	FormatState temp = state.split();
	state << ")";
	state = std::move(temp);
}

#define STREAM(os,code) [&]() -> std::string{ std::ostringstream os; code; return std::move(os).str(); }()

template<bool atomic>
static void formatValue(RewriteState& rs, FormatState state, const Value& value);

static void formatBinding(RewriteState& rs, FormatState state, const nix::Attr& attr){
	newLine(rs, state);

	state << STREAM(os,nix::printAttributeName(os, rs.eval().symbols[attr.name]));
	state << " = ";

	formatValue<false>(rs, state.split(), *attr.value);
	state << ";";
}

template<bool atomic>
static void formatValue(RewriteState& rs, FormatState state, const Value& value){
begin:
	switch(internalType(value)){
		case nix::tBool:
			if(value.boolean()){
				state << "true";
			}else{
				state << "false";
			}
			break;
		case nix::tString:
			if(value.context()){
				rs.eval().error<nix::EvalError>("cannot serialize string with context").debugThrow();
			}
			state << STREAM(os,nix::printLiteralString(os, value.string_view()));
			break;
		case nix::tNull:
			state << "null";
			break;
		case nix::tPath:{
			nix::SourcePath sp = value.path();
			if(sp.accessor == rs.basePath.accessor){
				const std::string& base = rs.basePath.path.abs();
				const std::string& subp = sp.path.abs();
				if(subp.starts_with(base)){
					state << "." << std::move(subp).substr(base.size());
				}else{
					state << sp.to_string();
				}
			}else{
				state << sp.to_string();
			}
		}break;

		case nix::tInt:
			if(atomic && value.integer().value < 0) addBrackets(rs, state);
			state << std::to_string(value.integer().value);

			break;
		
		case nix::tFloat:{
			// Why is this so complicated?
			auto f = value.fpoint();
			if(atomic && (f < 0.0 || std::isnan(f) || std::isinf(f))) addBrackets(rs, state);
			if(std::isnan(f)){
				state << "1.e308*2*0";
			}else if(std::isinf(f)){
				if(std::signbit(f)){
					state << "-1.e308*2";
				}else{
					state << "1.e308*2";
				}
			}else{
				state << std::format("{:-#}", f);
			}
		}break;
		
		case nix::tAttrs:{
			const Bindings& bindings = *value.attrs();
			if(bindings.size() == 0){
				state << "{}";
			}else{
				state << "{";

				state.indentInfo.level++;
				for(auto& binding : bindings){
					formatBinding(rs, state.split(), binding);
				}

				state.indentInfo.level--;
				newLine(rs,state);
				state << "}";
			}
		}break;
		
		case nix::tListN:
		case nix::tListSmall:{
			nix::ListView list = value.listView();
			if(list.size() == 0){
				state << "[]";
			}else{
				state << "[";

				state.indentInfo.level++;
				bool notFirst = false;
				for(auto& val : list){
					if(!newLine(rs, state) && notFirst){
						state << " ";
					}
					formatValue<true>(rs, state.split(), *val);
					notFirst = true;
				}

				state.indentInfo.level--;
				newLine(rs, state);
				state << "]";
			}
		}break;

		case nix::tPrimOp:{
			const Value& btin = rs.eval().getBuiltins();
			for(auto& attr : *btin.attrs()){
				if(NixValueComparer::eq(&value, attr.value)){
					state << "builtins." << STREAM(os, nix::printAttributeName(os, rs.eval().symbols[attr.name]));
					return;
				}
			}
			rs.eval().error<nix::EvalError>("Could not resolve primitive operator %1%", value.primOp()->name).debugThrow();
		}break;
		
		case nix::tPrimOpApp:{
			if(atomic) addBrackets(rs, state);
			formatValue<false>(rs, state.split(), *value.primOpApp().left);
			state << " ";
			formatValue<true>(rs, state.split(), *value.primOpApp().right);
		}break;
		
		case nix::tThunk:
		case nix::tUninitialized:
		case nix::tApp:
			rs.eval().error<nix::EvalError>("unexpected value").panic();
			break;
		case nix::tExternal:
		case nix::tFailed:
		case nix::tLambda:
		case nix::tNumberOfInternalTypes:
			rs.eval().error<nix::EvalError>("cannot serialize value").debugThrow();
			break;
			break;
	}
}

// Find or create the body of the attribute set represented by the specified expression. The state will be positioned at the end of a line and the caller is expected to add a new line immediately.
static FormatState findOrDeclareAttributeSet(RewriteState& state, const SyntaxReference& src, AnchorSet& anchors, bool ensureRecursive){
	const NixToken* cursor;
	if(src.isIsolated()){
		cursor = AttrsOpenInfo::nextOpen(src.boundary.data()).getOpenBracket();
		goto positionInsideSet;
	}else{
		std::vector<AttributeDeclaration> decvec = src.findAllDeclarations();
		for(const AttributeDeclaration& dec : decvec){
			// We know for a fact this can only be a binding info, because src is an attribute set.
			const BindingInfo& info = std::get<BindingInfo>(dec);
			if(info.getDoteq()->type == '='){
				cursor = AttrsOpenInfo::nextOpen(info.getDoteq() + 1).getOpenBracket();
				goto positionInsideSet;
			}
			if(ensureRecursive){
				break;
			}
		}
		// We also know for a fact there is at least one binding. SyntaxReference wouldn't exist otherwise.
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);
		const NixToken* insertAfter = info.getFirst() - 1;

		anchors.addAnchorForInsertLocation(insertAfter->end);
		
		FormatState body = addRewrite(state, insertAfter->end);
		body.indentInfo = detectIndentation(state, src.boundary.front().begin, src.boundary.back().end, info.getFirst()->begin);

		newLine(state, body);
		body << state.sourceSpan(info.getFirst()->begin, (info.getDoteq() - 1)->end);
		if(ensureRecursive){
			body << " = rec {";
		}else{
			body << " = {";
		}
		body.indentInfo.level++;

		FormatState inner = body.split();
		body.indentInfo.level--;
		newLine(state, body);
		body << "};";

		return std::move(inner);
	}
positionInsideSet:
	if(ensureRecursive && !src.attrs()->recursive){
		FormatState rfs = addRewrite(state, (cursor - 1)->end, cursor->begin);
		rfs << " rec ";
	}
	const NixToken* begin = cursor;
	maybeWalkToClosingToken(cursor);
	const NixToken* end = cursor;
	const NixToken* insertAfter = end - 1;

	anchors.addAnchorForInsertLocation(insertAfter->end);

	FormatState body = addRewrite(state, insertAfter->end);
	body.indentInfo = detectIndentation(state, begin->begin, end->end, insertAfter->end);

	if(begin->end == end->begin){
		FormatState inner = body.split();
		newLine(state, body);
		inner.indentInfo.level++;
		return inner;
	}else{
		return body;
	}
}

// Prepare for completely rewriting the syntax. The syntax may or may not be isolated. It SHOULD NOT be an inherit/inherit-from syntax.
static FormatState replaceSyntax(RewriteState& state, SyntaxReference src, AnchorSet& anchors){
	if(anchors.tryIsolateAndAnchor(src)){
		return addRewrite(state, src.beginOffset(), src.endOffset());
	}else{
		std::vector<AttributeDeclaration> decvec = src.findAllDeclarations();
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);

		anchors.addAnchorForBinding(info, src);

		FormatState fs;
		if(info.getDoteq()->type != '='){
			fs = addRewrite(state, (info.getDoteq() - 1)->end, info.getLast()->begin);
			fs << " = ";
		}else{
			fs = addRewrite(state, (info.getDoteq() + 1)->begin, info.getLast()->begin);
		}
		fs.indentInfo = detectIndentation(state, src.boundary.begin()->begin, src.boundary.end()->end, info.getFirst()->begin);

		return fs;
	}
}

void generatePositiveRewrites(RewriteState& state, SyntaxReference src, const Value& value, AnchorSet& anchors);

template<bool mayMakeRecursive,typename BindingsT>
void smartRewriteAttrs(RewriteState& state, SyntaxReference& src, const BindingsT& bindings, AnchorSet& anchors){
	FormatState body;
	ExprAttrs* a = src.attrs();
	// If we are adding new attributes, we must declare the attribute set first.
	for(auto& attr : bindings){
		if(!a->attrs->contains(attr.name)){
			body = findOrDeclareAttributeSet(state, src, anchors, false);
			break;
		}
	}
	for(auto& attr : bindings){
		if(a->attrs->contains(attr.name) && a->attrs->at(attr.name).chooseByKind(true, false, false)){
			SubexpressionFrame frame;
			generatePositiveRewrites(state, src.getSubexpression(&frame, attr.name), *attr.value, anchors);
		}else{
			newLine(state, body);
			body << STREAM(os,nix::printAttributeName(os, state.eval().symbols[attr.name])) << " = ";
			formatValue<false>(state, body.split(), *attr.value);
			body << ";";
		}
	}
}

static void eraseDeclaration(RewriteState& state, const AttrsDeclarationInfo& info){
	eraseRange(state, (info.getFirst() - 1)->end, info.getLast()->end);
}

void generateNegativeRewrites(RewriteState &state, AttrsOpenInfo info, uint32_t depth, const AnchorSet &anchors){

#define RANGE_HIT(itr,end_i) ( itr != anchors.end() && itr->first < end_i )

	AttrsSyntaxBase prev = info;
	while(true){
	continueOuterLoop:
		std::optional<AttributeDeclaration> dec = std::visit(nix::overloaded{
			[](AttrsCloseInfo) { return (std::optional<AttributeDeclaration>)std::nullopt; },
			[](AttributeDeclaration ad) { return std::optional<AttributeDeclaration>(ad); }
		}, prev.getNext());
		if(!dec.has_value()){
			return;
		}
		AttributeDeclaration ad = dec.value();

		AttrsDeclarationInfo& baseInfo = std::visit([](auto& a) -> AttrsDeclarationInfo& { return a; }, ad);
		prev = baseInfo;

		auto itr = anchors.upper_bound(baseInfo.getFirst()->begin);

		if(!RANGE_HIT(itr,baseInfo.getLast()->end)){
			eraseDeclaration(state, baseInfo);
			goto continueOuterLoop;
		}
		// We are keeping this. Look for subdeclarations.

		const NixToken* cursor = std::visit(nix::overloaded{
			[](BindingInfo& binding){ return (const NixToken*)nullptr; },
			[](InheritInfo& info){ return info.getAttrsBegin(); },
			[](InheritFromInfo& info){ return info.getAttrsBegin(); }
		}, ad);

		if(cursor == nullptr){
			BindingInfo& binding = std::get<BindingInfo>(ad);
			uint32_t innerDepth = depth + 1;

			while(binding.getDoteq()->type == '.'){
				binding = binding.getMember();
				innerDepth++;
			}

			do{
				if(innerDepth < itr->second){
					generateNegativeRewrites(state, binding.getAttrsValue(), innerDepth, anchors);
					goto continueOuterLoop;
				}
				itr++;
			}while(RANGE_HIT(itr,binding.getLast()->end));
		}else while(cursor != baseInfo.getLast()){
			itr = anchors.lower_bound(cursor->begin);
			if(!RANGE_HIT(itr, cursor->end)){
				eraseRange(state, (cursor - 1)->end, cursor->end);
			}
			cursor++;
		}
	}
};

void generatePositiveRewrites(RewriteState& state, SyntaxReference expr, const Value& value, AnchorSet& anchors){
	if(value.type() == nix::nAttrs){
		const ExprAttrs* attrs = dynamic_cast<ExprAttrs*>(expr.expression);
		if(!attrs){
			goto defaultRewrite;
		}
		const Bindings& bindings = *value.attrs();
		if(bindings.empty()){
			goto defaultRewrite;
		}
		smartRewriteAttrs<true>(state, expr, bindings, anchors);
		return;
	}

defaultRewrite:

	formatValue<false>(state, replaceSyntax(state, std::move(expr), anchors), value);
}

/*void generatePreservingAnchors(RewriteState& state, SyntaxReference expr, AnchorSet& anchors){

	uint32_t depthBeforeIsolation = expr.getPathLength();
	if(expr.tryIsolate()){
		anchors.addAnchor(expr.beginOffset(), depthBeforeIsolation);
	}else{
		for(auto d : expr.findAllDeclarations(state.eval())){
			if(std::holds_alternative<BindingInfo>(d)){
				BindingInfo bin = std::get<BindingInfo>(d);
				anchors.addAnchor(bin.getDoteq()->end, depthBeforeIsolation);
			}else{
				const NixToken* tok = expr.binarySearchPos(expr.expression->getPos());
				anchors.addAnchor(tok->begin, depthBeforeIsolation);
			}
		}
	}
}*/

/* This remains unused until we DARE descend lists.
// This ASSUMES the expr is isolated!
static void generateTopLevelRewrites(RewriteState& state, SyntaxReference expr, const Value& value){
	if(value.type() == nix::nAttrs){
		const ExprAttrs* attrs = dynamic_cast<ExprAttrs*>(expr.expression);
		if(!attrs){
			goto defaultRewrite;
		}
		const Bindings& bindings = *value.attrs();
		if(bindings.empty()){
			goto defaultRewrite;
		}
		AnchorSet anchors;
		generatePositiveRewrites(state, expr, value, anchors);
		generateNegativeRewrites(state, expr, anchors);
	}

defaultRewrite:
	
	formatValue<false>(state, addRewrite(state, expr.beginOffset(), expr.endOffset()), value);
}
*/

struct ConstAttr{
	nix::Symbol name;
	nix::PosIdx pos;
	const Value* value;
};

void generatePositiveRewrites(RewriteState& state, SyntaxReference& expr, const Bindings& bindings, AnchorSet& anchors){
	ExprAttrs* a = expr.attrs();
	std::vector<ConstAttr> attrs;

	for(auto& updateBinding : bindings){
		if(a->attrs->contains(updateBinding.name)){
			deeperForce(state.eval(), *updateBinding.value);
			attrs.push_back({updateBinding.name,updateBinding.pos,state.pool.intern(updateBinding.value)});
		}
	}

	std::sort(attrs.begin(),attrs.end(),[&](const auto& l,const auto& r){
		return l.pos < r.pos;
	});

	smartRewriteAttrs<false>(state, expr, attrs, anchors);
}

}