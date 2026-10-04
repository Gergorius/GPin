
#include "update.h"
#include "exprutil.h"
#include "steal.h"
#include "altparse.h"
#include "valueutil.h"
#include "format.h"

#include <boost/container_hash/hash.hpp>
#include <functional>
#include <memory>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/value.hh>
#include <nix/util/error.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/position.hh>
#include <nix/util/source-path.hh>
#include <nix/util/util.hh>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <ostream>
#include <string>
#include <variant>
#include <vector>

EXPORT_PRIVATE_MEMBER(getImportResolutionCache, &nix::EvalState::importResolutionCache);
EXPORT_PRIVATE_MEMBER(getFileEvalCache, &nix::EvalState::fileEvalCache);

struct RootValueMaker{
	EvalState& state;
	operator nix::RootValue(){
		return nix::allocRootValue(state.allocValue());
	}
};

void RecordedExprAttrs::eval(EvalState &state, Env &env, Value &v){
	EvalStateForUpdate& stateu = static_cast<EvalStateForUpdate&>(state);
	auto [itr, inserted] = stateu.valueMap.insert({this,nullptr});
	Value* target;
	if(inserted){
		[[unlikely]];
		target = state.allocValue();
		itr->second = nix::allocRootValue(target);
		target->mkThunk(&env, this);
	}else{
		target = *itr->second;
	}
	if(target->isThunk()){
		[[likely]];
		nix::ExprAttrs::eval(state, env, *target);
	}
	if(&v != target){
		[[unlikely]];
		v = *target;
	}
}

Value* RecordedExprAttrs::maybeThunk(EvalState& state, Env& env){
	EvalStateForUpdate& stateu = static_cast<EvalStateForUpdate&>(state);
	Value* val = state.allocValue();
	val->mkThunk(&env, this);
	if(!stateu.valueMap.insert({this, nix::allocRootValue(val)}).second){
		[[unlikely]];
		state.error<nix::EvalError>("RecordedExprAttrs::maybeThunk was called after a value was already created!").panic();
	}
	stateu.forceQueue.push_back(val);
	return val;
};

// Now it would be nice if we did not have to use object obtrusion. An idea I had is to make a temporary Exprs memory and later on move the expression tree to the permanent memory provided by EvalState. Nix helps with this: The bindVars implementation of some expressions will helpfully move data to the new memory. The problem is they do not move all of it. ExprLambda for example will leave it's formals in the old memory. Which is problematic if the old memory is temporary.

// We have three options:
// Use temporary memory anyway and special case expressions that do it poorly.
// Use the main memory and create custom copies of expressions in it. This will leak the memory occupied by the old objects.
// Use the main memory and replace expression objects with their modified variants.
// I chose the latter and hope I have enough static asserts in place so that I don't shoot my leg off later. Nix is not being helpful because some expressions' constructors are annoying or perform allocation. The RAII goblins work against us and our only saving grace are the move constructors that survived their onslaught.

// #define UPDATE_USE_RELOCATION // Do not. The implementation for this is incomplete. WILL crash.

struct RelocatingVisitor{
#ifdef UPDATE_USE_RELOCATION
	nix::Exprs& exprs;
	RelocatingVisitor(nix::Exprs& e): exprs(e){}
	void operator()(nix::Expr*) const{}
	template<typename T>
	void operator()(T* expr) const{
		expr = exprs.add<T>(std::move(*expr));
		visitSubexprs(expr,[&](nix::Expr* expr){ visitDynamicExpr(expr, *this); });
	}
#else
	void operator()(nix::Expr*) const{}
	RelocatingVisitor(nix::Exprs& e){}
#endif
};

struct ObtrusiveVisitor : private RelocatingVisitor{
	using RelocatingVisitor::RelocatingVisitor;
	const RelocatingVisitor& rv() const{
		return *this;
	}
	void operator()(nix::Expr* expr) const{ rv()(expr); }
	template<typename T> struct replacement{ using type = Greedy<T>; };
	template<> struct replacement<nix::ExprAttrs>{ using type = RecordedExprAttrs; };
#define SELF_REPLACE(name) template<> struct replacement<nix::name>{ using type = nix::name; }
	SELF_REPLACE(Expr);
	SELF_REPLACE(ExprFloat);
	SELF_REPLACE(ExprInt);
	SELF_REPLACE(ExprString);
	SELF_REPLACE(ExprVar);
	SELF_REPLACE(ExprPath);
	SELF_REPLACE(ExprInheritFrom);
	SELF_REPLACE(ExprBlackHole);
	template<typename T>
	void operator()(T* op) const{
		using TR = typename replacement<T>::type;
#ifdef UPDATE_USE_RELOCATION
		TR* ap = rv.exprs.add<TR>(std::move(*op));
#else
		TR* ap = obtrudeAsGreedy<T,TR>(op);
#endif
		if constexpr(std::is_same_v<T, nix::ExprLambda>){
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicExpr(expr, rv()); });
		}else{
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicExpr(expr, *this); });
		}
	}
};

nix::Value* processParseResult(EvalStateForUpdate& state, SourceInfo& info){
	visitDynamicExpr(info.parseResult.rootExpression, ObtrusiveVisitor{state.mem.exprs});
	info.parseResult.rootExpression->bindVars(state, state.staticBaseEnv);
	nix::Value* v = info.parseResult.rootExpression->maybeThunk(state,state.baseEnv);
	info.value = nix::allocRootValue(v);
	return v;
}

const SourceInfo& loadFileImpl(EvalStateForUpdate& state, const nix::SourcePath& path){
	auto& importResolutionCache = *std::invoke(getImportResolutionCache, state);
	auto& fileEvalCache = *std::invoke(getFileEvalCache, state);

	auto resolvedPath = nix::getConcurrent(importResolutionCache, path);

	if(!resolvedPath) {
		resolvedPath = resolveExprPath(path);
		importResolutionCache.emplace(path, *resolvedPath);
	}

	auto& file = resolvedPath.value();

	auto [itr, n] = state.fileSources.insert({file, SourceInfo(file.parent())});

	SourceInfo& info = itr->second;
	if(!n){
		return info;
	}

	info.content = std::make_shared<std::string>(file.readFile());

    nix::Pos::Origin origin = file;
#ifdef UPDATE_USE_RELOCATION
	nix::Exprs exprs;
#else
	nix::Exprs& exprs = state.mem.exprs;
#endif
	parseExprFromString(info.parseResult, state, origin, info.basePath, exprs, *info.content);
	fileEvalCache.emplace(file,processParseResult(state, info));
	return info;
}
const SourceInfo& EvalStateForUpdate::loadFile(const nix::SourcePath& path){
	return loadFileImpl(*this, path);
}

const SourceInfo& loadStringImpl(EvalStateForUpdate& state, nix::SourcePath basePath, std::string data, bool isStdin){
	SourceInfo& info = state.customSources.emplace_back(basePath);
	shared_ptr<std::string> dataPointer = std::make_shared<std::string>(data);
	info.content = dataPointer;

	nix::Pos::Origin origin = isStdin
		? nix::Pos::Origin(nix::Pos::Stdin (nix::ref<std::string>(dataPointer)))
		: nix::Pos::Origin(nix::Pos::String(nix::ref<std::string>(dataPointer)));
	
#ifdef UPDATE_USE_RELOCATION
	nix::Exprs exprs;
#else
	nix::Exprs& exprs = state.mem.exprs;
#endif
	parseExprFromString(info.parseResult, state, origin, info.basePath, exprs, *info.content);
	processParseResult(state, info);

	return info;
}
const SourceInfo& EvalStateForUpdate::loadString(nix::SourcePath basePath, std::string data, bool isStdin){
	return loadStringImpl(*this, basePath, data, isStdin);
}

void EvalStateForUpdate::finishLoad(){
	while(!forceQueue.empty()){
		Value* val = forceQueue.back();
		forceQueue.back() = nullptr;
		forceQueue.pop_back();
		forceValue(*val, nix::noPos);
	}
}

namespace{
	SyntaxReference& descendHelper(RecordedExprAttrs* a, SyntaxReference& sr){
		return sr;
	}
	SyntaxReference descendHelper(RecordedExprAttrs* a, RawSyntaxReference& sr){
		return sr.descendToIsolated(a);
	}
}

template<typename SynRef>
void recursiveRewrite(RewriteState& state, SynRef sr){
	constexpr bool raw = std::is_same_v<SynRef, RawSyntaxReference>;
	if constexpr(!raw){
		sr.tryIsolate();
	}
	EvalStateForUpdate& ext = static_cast<EvalStateForUpdate&>(state.eval());
	if(auto a = dynamic_cast<RecordedExprAttrs*>(sr.expression)){

		decltype(auto) syntax = descendHelper(a, sr);

		const nix::Bindings* myBindings = (**ext.valueMap[a]).attrs();
		const nix::Bindings* updateBindings = nullptr;

		const nix::Attr* updater = myBindings->get(ext.updateSymbol);

		if(updater != nullptr){

			state.eval().forceAttrs(*updater->value, updater->pos, "while evaluating updater");

			updateBindings = updater->value->attrs();

			generateUpdate(state, syntax, *updateBindings);
		}
		RawSyntaxReference rsr = syntax;

		if(a->inheritFromExprs){
			for(auto& def : *a->inheritFromExprs.get()){
				rsr.expression = def;
				recursiveRewrite(state, rsr);
			}
		}
		for(auto& def : a->attrs.value()){
			// For obvious reasons we ignore bindings that are being updated.
			if(updateBindings == nullptr || updateBindings->get(def.first) == nullptr){
				SubexpressionFrame frame;
				recursiveRewrite(state, syntax.getSubexpression(&frame, def.first));
			}
		}
		for(uint32_t i = 0;i < a->dynamicAttrs->size();i++){
			rsr.expression = a->dynamicAttrs->at(i).nameExpr;
			recursiveRewrite(state, rsr);
			
			SubexpressionFrame frame;
			recursiveRewrite(state, syntax.getDynamicSubexpression(&frame, i));
		}
	}else{
		visitDynamicExpr(sr.expression, mkSubexprs([&](nix::Expr* ex){
			RawSyntaxReference rsr = sr;
			rsr.expression = ex;
			recursiveRewrite(state, rsr);
		}));
	}
}

void EvalStateForUpdate::doRewrite(std::ostream& out, const SourceInfo& info){
	RewriteState rewrite{
		.pool = this->pool,
		.defaultIndent = "",
		.source = *info.content,
		.basePath = info.basePath
	};

	SyntaxReference root = RawSyntaxReference{
		.origin = *info.parseResult.origin,
		.boundary = info.parseResult.tokens,
		.pathLength = 0,
		.path = nullptr,
		.expression = info.parseResult.rootExpression,
	};

	recursiveRewrite(rewrite, std::move(root));

	std::vector<Rewrite>& rwvec = rewrite.rewrites;

	std::sort(rwvec.begin(),rwvec.end());

	uint32_t cursor = 0;

	for(Rewrite& rw : rwvec){
		out << string_view(*info.content).substr(cursor, rw.begin - cursor);
		out << rw;
		cursor = rw.end;
	}

	out << string_view(*info.content).substr(cursor);
}