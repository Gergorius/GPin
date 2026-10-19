
#include "format.h"
#include "altparse.h"
#include "treewalk.h"
#include "valueutil.h"

#include <algorithm>
#include <boost/multi_index_container_fwd.hpp>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
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
#include <unordered_map>
#include <utility>
#include <variant>
#include <boost/multi_index_container.hpp>

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

// TODO
// Handle unreferencable symbols in cycles
// Handle lists and primitive operator applications in cycles

namespace gpin{

// Copied over from print.cc.
static bool isVarName(std::string_view s){
	if (s.size() == 0)
		return false;
	if (nix::isReservedKeyword(s))
		return false;
	char c = s[0];
	if ((c >= '0' && c <= '9') || c == '-' || c == '\'')
		return false;
	for (auto & i : s)
		if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9') || i == '_' || i == '-'
				|| i == '\''))
			return false;
	return true;
}

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

struct Cursor{
	RewriteLink* targetLink;
	IndentInfo indentInfo;
public:
	Cursor(){}
	Cursor(std::unique_ptr<RewriteLink>& t){
		t.reset(targetLink = new RewriteLink());
	}
	Cursor(Cursor&& that){
		targetLink = that.targetLink;
		indentInfo = that.indentInfo;
		that.targetLink = nullptr;
	}
	Cursor(const Cursor&) = delete;
	Cursor& operator=(Cursor&& that){
		this->~Cursor();
		new (this) Cursor(std::move(that));
		return *this;
	}
	Cursor& operator=(const Cursor&) = delete;
private:
	RewriteLink& push(){
		RewriteLink* nl = new RewriteLink();
		nl->next = std::move(targetLink->next);
		targetLink->next.reset(nl);
		targetLink = nl;
		return *nl;
	}
public:
	// Split the insertion cursor into two. Everything added to the new returned format state will precede anything subsequently added to this format state. The lifetimes of the states are not interrelated.
	Cursor split(){
		Cursor temp;
		temp.targetLink = targetLink;
		temp.indentInfo = indentInfo;
		push();
		return temp;
	}
	Cursor& operator<<(string str){
		if(str.empty()){ return *this; }
		if(targetLink->view().empty()){
			targetLink->data = std::move(str);
			return *this;
		}
		if(std::holds_alternative<string>(targetLink->data)){
			std::string& s = std::get<string>(targetLink->data);
			if(s.size() + str.size() <= s.capacity()){
				s += std::move(str);
				return *this;
			}
		}
		push().data = std::move(str);
		return *this;
	}
	Cursor& operator<<(string_view view){
		if(view.empty()){ return *this; }
		if(targetLink->view().empty()){
			targetLink->data = view;
			return *this;
		}
		if(view.size() < 0x10 && std::holds_alternative<string>(targetLink->data)){
			std::string& s = std::get<string>(targetLink->data);
			if(s.size() + view.size() <= s.capacity()){
				s += view;
				return *this;
			}
		}
		push().data = view;
		return *this;
	}
	template<size_t N>
	Cursor& operator<<(const char (&str)[N]){
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

struct BasicChartValueInfo{
	const BasicChartValueInfo* pathParent = nullptr;
	nix::Symbol pathName = nix::Symbol();
	BasicChartValueInfo(){}
	const BasicChartValueInfo* asPtr() const;
};

static const BasicChartValueInfo root_store = BasicChartValueInfo();
static const BasicChartValueInfo* path_unreachable = nullptr;
static const BasicChartValueInfo* path_env = &root_store;

const BasicChartValueInfo* BasicChartValueInfo::asPtr() const{ return pathParent == path_unreachable ? path_unreachable : this; }

bool samePath(const BasicChartValueInfo* left, const BasicChartValueInfo* right){
	if(left == right){
		return true;
	}
	if(left == path_env || right == path_env || left == path_unreachable || right == path_unreachable){
		return false;
	}
	return left->pathName == right->pathName && samePath(left->pathParent, right->pathParent);
}

struct ChartValueInfo : BasicChartValueInfo{
	uint32_t pathCost = 0xFFFFFFFFu;
	ChartValueInfo(){}
};

struct ChartContext{
	RewriteState& state;
	std::unordered_map<const Value*, ChartValueInfo>& paths;
};

static void chart(ChartContext& context, const Value* value, ChartValueInfo& info){
	if(value->type() != nix::nAttrs){
		return;
	}
	for(auto& bnd : *value->attrs()){
		const nix::Value* subValue = bnd.value;

		ChartValueInfo& subInfo = context.paths[subValue];

		uint32_t subPathCost = info.pathCost + context.state.eval().symbols[bnd.name].size() + 1;

		if(subPathCost < subInfo.pathCost){
			subInfo.pathParent = &info;
			subInfo.pathCost = subPathCost;
			subInfo.pathName = bnd.name;
			chart(context, subValue, subInfo);
		}
	}
}

// Reverse environment used for substituting values.
struct FormatEnv{
	friend const ChartValueInfo* lookup(const FormatEnv* env, const Value* vptr);
	friend bool wasMadeRecursive(const FormatEnv* env, const Value* vptr);
	const Value* definedForValue;
private:
	using map_type = std::unordered_map<nix::Symbol, std::unordered_map<const Value*, ChartValueInfo>>;
	map_type data;
	const FormatEnv* parent;
	struct Inhibitor{
		const Inhibitor* parent;
		const map_type& map;
	};
	void lookupBestReference(const Value* vptr, const Inhibitor* inhibitor, const ChartValueInfo*& bestMatch) const{
		for(auto& [sym, map] : data){
			auto itr = map.find(vptr);
			if(itr == map.end()){
				continue;
			}
			const ChartValueInfo* info = &itr->second;
			const Inhibitor* li = inhibitor;
			while(li != nullptr){
				if(li->map.contains(sym)){
					goto continueOuter;
				}
				li = li->parent;
			}
			if(bestMatch == nullptr || info->pathCost < bestMatch->pathCost){
				bestMatch = info;
			}
		continueOuter:;
		}
		if(parent != nullptr){
			Inhibitor ti{.parent = inhibitor, .map = data};
			parent->lookupBestReference(vptr, &ti, bestMatch);
		}
	}
public:
	FormatEnv(): parent(nullptr){}
	FormatEnv(const FormatEnv* p): parent(p){}
	bool addSymbol(RewriteState& state, nix::Symbol symbol, const Value* value){
		auto itr = data.insert({symbol, map_type::mapped_type{}});
		std::unordered_map<const Value*, ChartValueInfo>& paths = itr.first->second;
		if(itr.second){
			string_view symbolName = state.eval().symbols[symbol];

			// The first symbol must be a var name, otherwise we won't be able to reference it. The rest of the symbols can be whatever.
			if(isVarName(symbolName)){
				ChartValueInfo& i = paths.insert({value, ChartValueInfo{}}).first->second;
				i.pathParent = path_env;
				i.pathName = symbol;
				i.pathCost = symbolName.size();
				ChartContext context = ChartContext{
					.state = state,
					.paths = paths
				};
				chart(context, value, i);
			}
		}
		return !paths.empty();
	}
};

// Was this value already made recursive?
bool wasMadeRecursive(const FormatEnv* env, const Value* vptr){
	while(env != nullptr){
		if(env->definedForValue == vptr){
			return true;
		}
		env = env->parent;
	}
	return false;
}

const ChartValueInfo* lookup(const FormatEnv* env, const Value* vptr){
	const ChartValueInfo* ref = nullptr;
	if(env != nullptr){
		env->lookupBestReference(vptr, nullptr, ref);
	}
	return ref;
}

template<bool mayChangeRecursiveness,typename BindingsT>
bool maybeMakeFormatEnv(RewriteState& state, ExprAttrs* attrs, FormatEnv& subEnv, const FormatEnv*& env, const BindingsT& bindings, const Value* vptr){

	bool wantRecursive = false;
	if(mayChangeRecursiveness){
		if(wasMadeRecursive(env, vptr)){
			return false;
		}
		for(auto& attr : bindings){
			if(mayChangeRecursiveness && !wantRecursive && state.cycles.checkForCycle(attr.value)){
				wantRecursive = true;
			}
		}
	}else{
		wantRecursive = attrs->recursive;
	}
	if(wantRecursive){
		subEnv = FormatEnv(env);
		subEnv.definedForValue = vptr;
		for(auto& attr : bindings){
			subEnv.addSymbol(state, attr.name, attr.value);
		}
		env = &subEnv;
	}
	return wantRecursive;
}

static Cursor addRewrite(RewriteState& rs,uint32_t begin,uint32_t end){
#ifndef NDEBUG
	return Cursor(rs.replacement.insert({RewriteRange{begin, end, rs.offsetToNixPos(begin)}, nullptr})->second);
#else
	return Cursor(rs.replacement.insert({RewriteRange{begin, end}, nullptr})->second);
#endif
}

static Cursor addRewrite(RewriteState& rs,uint32_t begin){
	return addRewrite(rs, begin, begin);
}

static void eraseRange(RewriteState& rs,uint32_t begin,uint32_t end){
#ifndef NDEBUG
	rs.replacement.insert({RewriteRange{begin, end, rs.offsetToNixPos(begin)}, nullptr});
#else
	rs.replacement.insert({RewriteRange{begin, end}, nullptr});
#endif
}

static bool newLine(RewriteState& rs, Cursor& fs){
	if(fs.indentInfo.indent.size() == 0){
		return false; // No new lines.
	}
	fs << "\n" << fs.indentInfo.preindent;
	for(int32_t i = 0;i < fs.indentInfo.level;i++){
		fs << fs.indentInfo.indent;
	}
	return true;
}

static void addBrackets(RewriteState& rs, Cursor& state){
	state << "(";
	Cursor temp = state.split();
	state << ")";
	state = std::move(temp);
}

#define STREAM(os,code) [&]() -> std::string{ std::ostringstream os; code; return std::move(os).str(); }()

static void formatPath(RewriteState& rs, Cursor state, const BasicChartValueInfo* path){
	while(path != path_env){
		Cursor temp;
		if(path->pathParent != path_env){
			temp = state.split();
			state << ".";
		}
		state << STREAM(os,nix::printAttributeName(os, rs.eval().symbols[path->pathName]));
		state = std::move(temp);
		path = path->pathParent;
	}
};

template<bool atomic>
static void formatValue(RewriteState& rs, const BasicChartValueInfo* path, const FormatEnv* env, Cursor state, const Value& value);

static void formatBinding(RewriteState& rs, const BasicChartValueInfo* path, const FormatEnv* env, Cursor state, const nix::Attr& attr){
	newLine(rs, state);

	BasicChartValueInfo subInfo;
	subInfo.pathParent = path;
	subInfo.pathName = attr.name;

	state << STREAM(os,nix::printAttributeName(os, rs.eval().symbols[attr.name]));
	state << " = ";

	formatValue<false>(rs, subInfo.asPtr(), env, state.split(), *attr.value);
	state << ";";
}

static bool possiblyFormatLookup(RewriteState& rs, const BasicChartValueInfo* path, const FormatEnv* env, Cursor& state, const Value& value){
	const BasicChartValueInfo* lookupPath = lookup(env, &value);
	if(lookupPath != path_unreachable && !samePath(lookupPath, path)){
		formatPath(rs, std::move(state), lookupPath);
		return true;
	}else{
		return false;
	}
}

template<bool atomic>
static void formatValue(RewriteState& rs, const BasicChartValueInfo* path, const FormatEnv* env, Cursor state, const Value& value){
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
				Value temp = value;
				rs.eval().forceStringNoCtx(temp, nix::noPos, "while trying to update an attribute");
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
			}else if(!possiblyFormatLookup(rs, path, env, state, value)){

				FormatEnv localEnv;
				bool wantRecursive = maybeMakeFormatEnv<true>(rs, nullptr, localEnv, env, bindings, &value);

				if(wantRecursive){
					path = path_env; // We become the environment.
					state << "rec {";
				}else{
					state << "{";
				}

				state.indentInfo.level++;
				for(auto& binding : bindings){
					formatBinding(rs, path, env, state.split(), binding);
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
			}else if(!possiblyFormatLookup(rs, path, env, state, value)){
				state << "[";

				state.indentInfo.level++;
				bool notFirst = false;
				for(auto& val : list){
					if(!newLine(rs, state) && notFirst){
						state << " ";
					}
					formatValue<true>(rs, path_unreachable, env, state.split(), *val);
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
			rs.eval().error<nix::EvalError>("could not resolve primitive operator %1%", value.primOp()->name).debugThrow();
		}break;
		
		case nix::tPrimOpApp:
		case nix::tApp:{

			if(possiblyFormatLookup(rs, path, env, state, value)) break;

			if(atomic) addBrackets(rs, state);

			const Value* vptr = &value;
			do{
				Cursor temp = state.split();
				state << " ";
				nix::Value* r;
				nix::Value* l;
				if(internalType(*vptr) == nix::tPrimOpApp){
					r = value.primOpApp().right;
					l = value.primOpApp().left;
				}else{
					r = value.app().right;
					l = value.app().left;
				}
				formatValue<true>(rs, path_unreachable, env, std::move(state), *r);
				state = std::move(temp);
				vptr = l;
			}while(internalType(*vptr) == nix::tPrimOpApp || internalType(*vptr) == nix::tApp);

			formatValue<false>(rs, path_unreachable, env, std::move(state), *vptr);
		}break;
		
		case nix::tThunk:
		case nix::tUninitialized:
		case nix::tExternal:
		case nix::tFailed:
		case nix::tLambda:
		case nix::tNumberOfInternalTypes:
			rs.eval().error<nix::EvalError>("cannot serialize value").debugThrow();
			break;
	}
}

// Find or create the body of the attribute set represented by the specified expression. The state will be positioned at the end of a line and the caller is expected to add a new line immediately.
static Cursor findOrDeclareAttributeSet(RewriteState& state, const SyntaxReference& src, AnchorSet& anchors, bool recursive, bool forAdding){
	const NixToken* cursor;
	std::vector<AttributeDeclaration> decvec{};
	nix::ExprAttrs* attrs = static_cast<nix::ExprAttrs*>(src.expression);
	if(recursive != attrs->recursive){
		decvec = src.findAllDeclarations();
		for(const AttributeDeclaration& dec : decvec){
			const BindingInfo& info = std::get<BindingInfo>(dec);
			if(info.getDoteq()->type == '='){
				cursor = AttrsOpenInfo::nextOpen(info.getDoteq() + 1).getOpenBracket();
				if(recursive){
					if((cursor - 1)->type != NixToken::kind_type::REC){
						Cursor rec = addRewrite(state, cursor->begin);
						rec << " rec ";
					}
				}else{
					if((cursor - 1)->type == NixToken::kind_type::REC){
						eraseRange(state, (cursor - 1)->begin, cursor->begin);
					}
				}
			}
		}
	}
	if(src.isIsolated()){
		cursor = AttrsOpenInfo::nextOpen(src.boundary.data()).getOpenBracket();
		goto positionInsideSet;
	}else{
		if(recursive == attrs->recursive){
			decvec = src.findAllDeclarations();
		}
		for(const AttributeDeclaration& dec : decvec){
			// We know for a fact this can only be a binding info, because src is an attribute set.
			const BindingInfo& info = std::get<BindingInfo>(dec);
			if(info.getDoteq()->type == '='){
				cursor = AttrsOpenInfo::nextOpen(info.getDoteq() + 1).getOpenBracket();
				goto positionInsideSet;
			}
			if(recursive){
				// The very first declaration is not a set declaration but it has to be for recursive to work.
				break;
			}
		}
		// We also know for a fact there is at least one binding. SyntaxReference wouldn't exist otherwise.
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);
		const NixToken* insertAfter = info.getFirst() - 1;

		anchors.addAnchorForInsertLocation(insertAfter->end);
		
		Cursor body = addRewrite(state, insertAfter->end);
		body.indentInfo = detectIndentation(state, src.boundary.front().begin, src.boundary.back().end, info.getFirst()->begin);

		newLine(state, body);
		body << state.sourceSpan(info.getFirst()->begin, (info.getDoteq() - 1)->end);
		if(recursive){
			body << " = rec {";
		}else{
			body << " = {";
		}
		if(!forAdding){
			body << "};";
			return Cursor();
		}
		body.indentInfo.level++;

		Cursor inner = body.split();
		body.indentInfo.level--;
		newLine(state, body);
		body << "};";

		return std::move(inner);
	}
positionInsideSet:
	const NixToken* begin = cursor;
	maybeWalkToClosingToken(cursor);
	const NixToken* end = cursor;
	const NixToken* insertAfter = end - 1;

	anchors.addAnchorForInsertLocation(insertAfter->end);

	if(!forAdding){
		return Cursor();
	}

	Cursor body = addRewrite(state, insertAfter->end);
	body.indentInfo = detectIndentation(state, begin->begin, end->end, insertAfter->end);

	if(begin->end == end->begin){
		Cursor inner = body.split();
		newLine(state, body);
		inner.indentInfo.level++;
		return inner;
	}else{
		return body;
	}
}

// Prepare for completely rewriting the syntax. The syntax may or may not be isolated. It SHOULD NOT be an inherit/inherit-from syntax.
static Cursor replaceSyntax(RewriteState& state, SyntaxReference src, AnchorSet& anchors){
	if(anchors.tryIsolateAndAnchor(src)){
		return addRewrite(state, src.beginOffset(), src.endOffset());
	}else{
		std::vector<AttributeDeclaration> decvec = src.findAllDeclarations();
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);

		anchors.addAnchorForBinding(info, src);

		Cursor fs;
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

void generatePositiveRewrites(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, SyntaxReference src, const Value& value, AnchorSet& anchors);

template<bool mayChangeRecursiveness,typename BindingsT>
void smartRewriteAttrs(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, SyntaxReference& src, const BindingsT& bindings, const Value* value, AnchorSet& anchors){
	Cursor body;
	ExprAttrs* a = src.attrs();

	// We may change a recursive attrs to a non-recursive one because it is possible we were the original authors... except when we aren't! Which is why that template argument exists.

	FormatEnv localEnv;
	bool wantNew = false;
	bool wantRecursive = maybeMakeFormatEnv<mayChangeRecursiveness>(state, a, localEnv, env, bindings, value);

	// If we are adding new attributes, we must declare the attribute set first. This is due to rewrite ordering messing with the "rec" marker.
	for(auto& attr : bindings){
		wantNew |= !a->attrs->contains(attr.name);
	}
	if(wantNew || (wantRecursive != a->recursive)){
		body = findOrDeclareAttributeSet(state, src, anchors, wantRecursive, wantNew);
	}
	for(auto& attr : bindings){
		BasicChartValueInfo subInfo;
		subInfo.pathParent = wantRecursive ? path_env : path;
		subInfo.pathName = attr.name;
		if(a->attrs->contains(attr.name) && a->attrs->at(attr.name).chooseByKind(true, false, false)){
			SubexpressionFrame frame;
			generatePositiveRewrites(state, subInfo.asPtr(), env, src.getSubexpression(&frame, attr.name), *attr.value, anchors);
		}else{
			newLine(state, body);
			body << STREAM(os,nix::printAttributeName(os, state.eval().symbols[attr.name])) << " = ";
			formatValue<false>(state, subInfo.asPtr(), env, body.split(), *attr.value);
			body << ";";
		}
	}
}

static void eraseDeclaration(RewriteState& state, const AttrsDeclarationInfo& info){
	eraseRange(state, (info.getFirst() - 1)->end, info.getLast()->end);
}

void generatePositiveRewrites(RewriteState& state, const BasicChartValueInfo* path, const FormatEnv* env, SyntaxReference expr, const Value& value, AnchorSet& anchors){
	if(value.type() == nix::nAttrs){
		const ExprAttrs* attrs = dynamic_cast<ExprAttrs*>(expr.expression);
		if(!attrs){
			goto defaultRewrite;
		}
		const Bindings& bindings = *value.attrs();
		if(bindings.empty()){
			goto defaultRewrite;
		}
		smartRewriteAttrs<true>(state, path, env, expr, bindings, &value, anchors);
		return;
	}

defaultRewrite:

	formatValue<false>(state, path, env, replaceSyntax(state, std::move(expr), anchors), value);
}

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

	smartRewriteAttrs<false>(state, path_unreachable, nullptr, expr, attrs, nullptr, anchors);
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

		// We keep inherit-froms because they cannot always be anchored properly.

		if(!std::holds_alternative<InheritFromInfo>(ad) && !RANGE_HIT(itr,baseInfo.getLast()->end)){
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

}