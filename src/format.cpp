
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

struct FormatState{
	RewriteLink* cursor;
	string_view preindent;
	string_view indent;
	uint32_t indentRepeatCount = 0;
public:
	FormatState(){}
	FormatState(std::unique_ptr<RewriteLink>& t){
		t.reset(cursor = new RewriteLink());
	}
	FormatState(FormatState&& that){
		cursor = that.cursor;
		preindent = that.preindent;
		indent = that.indent;
		indentRepeatCount = that.indentRepeatCount;
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
	FormatState split(){
		FormatState temp;
		temp.cursor = cursor;
		temp.preindent = preindent;
		temp.indent = indent;
		temp.indentRepeatCount = indentRepeatCount;
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

#define GET_ADI [](const auto& v) -> const AttrsDeclarationInfo&{ return v; }

std::pair<string_view,string_view> detectIndentation(string_view source){
	string_view preindent = {};
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
	if(bestPair){
		return {preindent, bestPair->first};
	}else{
		return { {}, {} };
	}
}

static std::pair<string_view,string_view> detectIndentation(RewriteState& state, uint32_t begin, uint32_t end){
	using pos = string_view::size_type;
	pos lineStart = state.source.substr(0, begin).find_last_of('\n');
	lineStart = lineStart == string_view::npos ? 0 : lineStart + 1;
	return detectIndentation(state.source.substr(lineStart, end - lineStart));
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
	FormatState fs(rs.replacement.insert({{begin, end}, nullptr})->second);
	std::tie(fs.preindent,fs.indent) = detectIndentation(rs, begin, end);
	return std::move(fs);
}

static void eraseRange(RewriteState& rs,uint32_t begin,uint32_t end){
	rs.replacement.insert({{begin, end}, nullptr});
}

static bool newLine(RewriteState& rs, FormatState& fs){
	if(fs.indent.size() == 0){
		return false; // No new lines.
	}
	fs << "\n" << fs.preindent;
	for(int32_t i = 0;i < fs.indentRepeatCount;i++){
		fs << fs.indent;
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
				break;
			}
			state << "{";

			state.indentRepeatCount++;
			for(auto& binding : bindings){
				formatBinding(rs, state.split(), binding);
			}

			state.indentRepeatCount--;
			newLine(rs,state);
			state << "}";
		}break;
		
		case nix::tListN:
		case nix::tListSmall:{
			nix::ListView list = value.listView();
			if(list.size() == 0){
				state << "[]";
				break;
			}
			state << "[";

			state.indentRepeatCount++;
			bool notFirst = false;
			for(auto& val : list){
				if(!newLine(rs, state) && notFirst){
					state << " ";
				}
				formatValue<true>(rs, state.split(), *val);
				notFirst = true;
			}

			state.indentRepeatCount--;
			newLine(rs, state);
			state << "]";
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
			formatValue<true>(rs, std::move(state), *value.primOpApp().right);
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

static void eraseDeclaration(RewriteState& state, const AttrsDeclarationInfo& info){
	eraseRange(state, (info.begin - 1)->end, info.endsemi->end);
}

// Find or create the body of the attribute set represented by the specified expression. The state will be positioned at the end of a line and the caller is expected to add a new line immediately.
static FormatState findOrDeclareAttributeSet(RewriteState& state, const SyntaxReference& src, AnchorSet& anchors){
	std::span<const NixToken> boundary;
	const NixToken* cursor;
	if(src.isIsolated()){
		boundary = src.boundary;
		cursor = src.boundary.data();
		goto walkToBody;
	}
	{
		std::vector<AttributeDeclaration> decvec = src.findAllDeclarations(state.eval());
		for(uint32_t i = 0;i < decvec.size();i++){
			BindingInfo& info = std::get<BindingInfo>(decvec[i]);
			if(info.doteq->type == '='){
				boundary = { info.begin, info.endsemi };
				cursor = info.doteq + 1;
				goto walkToBody;
			}
		}

		FormatState fs = findOrDeclareAttributeSet(state, src.getParent(), anchors);
		newLine(state, fs);

		fs << STREAM(os,nix::printAttributeName(os, state.eval().symbols[src.path->name.symbol])) << " = {";
		FormatState body = fs.split();
		newLine(state, fs);
		fs << "};";

		body.indentRepeatCount++;
		return std::move(body);
	}
walkToBody:
	while(cursor->type != '{'){
		cursor++;
	}
	const NixToken* openBracket = cursor;
	anchors.addAnchorForAttrs(cursor->begin);

	maybeWalkToClosingToken(cursor);
	cursor--;

	FormatState fs = addRewrite(state, cursor->end, cursor->end);
	std::tie(fs.preindent,fs.indent) = detectIndentation(state, boundary[0].begin, boundary[boundary.size() - 1].end);
	fs.indentRepeatCount = 1;
	if(openBracket == cursor){
		FormatState tfs = fs.split();
		fs.indentRepeatCount--;
		newLine(state, fs);
		return tfs;
	}else{
		return fs;
	}
}

// Prepare for completely rewriting the syntax. The syntax may or may not be isolated. It SHOULD NOT be an inherit/inherit-from syntax.
static FormatState replaceSyntax(RewriteState& state, SyntaxReference src, AnchorSet& anchors){
	uint32_t depthBeforeIsolation = src.pathLength;
	if(src.tryIsolate()){
		anchors.addAnchor(src.beginOffset(), depthBeforeIsolation);
		return addRewrite(state, src.beginOffset(), src.endOffset());
	}else{
		std::vector<AttributeDeclaration> decvec = src.findAllDeclarations(state.eval());
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);

		anchors.addAnchor(info.doteq->end, src.pathLength);

		FormatState fs;
		if(info.doteq->type != '='){
			fs = addRewrite(state, (info.doteq - 1)->end, info.endsemi->begin);
			fs << " = ";
		}else{
			fs = addRewrite(state, (info.doteq + 1)->begin, info.endsemi->begin);
		}

		std::tie(fs.preindent, fs.indent) = detectIndentation(state, info.begin->begin, info.endsemi->end);

		return fs;
	}
}

void generatePositiveRewrites(RewriteState& state, SyntaxReference src, const Value& value, AnchorSet& anchors);

template<typename BindingsT>
void smartRewriteAttrs(RewriteState& state, SyntaxReference& src, const BindingsT& bindings, AnchorSet& anchors){
	std::optional<FormatState> newBindings;
	ExprAttrs* a = src.attrs();
	for(auto& attr : bindings){
		if(a->attrs->contains(attr.name) && a->attrs->at(attr.name).chooseByKind(true, false, false)){
			SubexpressionFrame frame;
			generatePositiveRewrites(state, src.getSubexpression(&frame, attr.name), *attr.value, anchors);
		}else{
			if(!newBindings){
				newBindings = findOrDeclareAttributeSet(state, src, anchors);
			}
			FormatState& fs = newBindings.value();
			newLine(state, fs);
			fs << STREAM(os,nix::printAttributeName(os, state.eval().symbols[attr.name])) << " = ";
			formatValue<false>(state, fs.split(), *attr.value);
			fs << ";";
		}
	}
}

// Erases EVERY declaration contained in the isolated ExprAttrs or ExprLet that isn't anchored.
void generateNegativeRewrites(RewriteState& state, SyntaxReference src, const AnchorSet& anchors){

#define RANGE_HIT(itr,end_i) ( itr != anchors.end() && itr->first < end_i )

	std::vector<std::pair<uint32_t,AttributeDeclaration>> vec;
	traverseAttrsDeclarations(src.boundary,[&vec](uint32_t,const auto& def){ vec.push_back({1, def}); return false; });

	while(!vec.empty()){
		auto [depth, d] = vec.back();
		vec.pop_back();
		const AttrsDeclarationInfo& dec = std::visit([](auto& a){ return (AttrsDeclarationInfo&)a; }, d);

		auto itr = anchors.lower_bound(dec.begin->begin);

		if(RANGE_HIT(itr, dec.endsemi->end)){
			const NixToken* cursor = std::visit([](auto& a){
				if constexpr(std::is_same_v<std::remove_cvref_t<decltype(a)>, BindingInfo>){
					return (const NixToken*)nullptr;
				}else{
					return a.getAttrsBegin();
				}
			}, d);
			if(cursor == nullptr){
				BindingInfo bin = std::get<BindingInfo>(d);
				while(bin.doteq->type == '.'){
					bin = bin.next();
					depth++;
				}
				while(RANGE_HIT(itr, dec.endsemi->end)){
					if(depth < itr->second){
						goto weNeedToGoDeeper;
					}
					itr++;
				}
				goto skipGoingDeeper;
			
			weNeedToGoDeeper:
				cursor = bin.doteq + 1;
				while(cursor->type != '{'){
					cursor++;
				}
				traverseAttrsDeclarations(
					{cursor, src.boundary.data() + src.boundary.size()},
					[&vec,depth](uint32_t,const auto& def){ vec.push_back({depth + 1, def}); return false; });
			skipGoingDeeper:

			}else while(cursor != dec.endsemi){
				auto itr2 = anchors.lower_bound(cursor->begin);
				if(!RANGE_HIT(itr2, cursor->end)){
					eraseRange(state, (cursor - 1)->end, cursor->end);
				}
				cursor++;
			}
		}else{
			eraseDeclaration(state, dec);
		}
	}
}

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
		smartRewriteAttrs(state, expr, bindings, anchors);
		return;
	}

defaultRewrite:

	formatValue<false>(state, replaceSyntax(state, std::move(expr), anchors), value);
}

void generatePreservingAnchors(RewriteState& state, SyntaxReference expr, AnchorSet& anchors){
	uint32_t depthBeforeIsolation = expr.pathLength;
	if(expr.tryIsolate()){
		anchors.addAnchor(expr.beginOffset(), depthBeforeIsolation);
	}else{
		for(auto d : expr.findAllDeclarations(state.eval())){
			if(std::holds_alternative<BindingInfo>(d)){
				BindingInfo bin = std::get<BindingInfo>(d);
				anchors.addAnchor(bin.doteq->end, depthBeforeIsolation);
			}else{
				const NixToken* tok = expr.binarySearchPos(expr.expression->getPos());
				anchors.addAnchor(tok->begin, depthBeforeIsolation);
			}
		}
	}
}

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

	smartRewriteAttrs(state, expr, attrs, anchors);
}

}