
#include "update.h"
#include "exprutil.h"
#include "steal.h"
#include "altparse.h"
#include "treewalk.h"
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
#include <vector>

namespace gpin{

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

#define RF return false

struct RelocatingVisitor{
#ifdef UPDATE_USE_RELOCATION
	nix::Exprs& exprs;
	RelocatingVisitor(nix::Exprs& e): exprs(e){}
	bool operator()(nix::Expr*) const{ RF; }
	template<typename T>
	bool operator()(T* expr) const{
		expr = exprs.add<T>(std::move(*expr));
		visitSubexprs(expr,[&](nix::Expr* expr){ visitDynamicExpr(expr, *this); RF; });
		RF;
	}
#else
	bool operator()(nix::Expr*) const{ RF; }
	RelocatingVisitor(nix::Exprs& e){}
#endif
};

struct ObtrusiveVisitor : private RelocatingVisitor{
	using RelocatingVisitor::RelocatingVisitor;
	const RelocatingVisitor& rv() const{
		return *this;
	}
	bool operator()(nix::Expr* expr) const{ rv()(expr); RF; }
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
	bool operator()(T*& op) const{
		using TR = typename replacement<T>::type;
#ifdef UPDATE_USE_RELOCATION
		TR* ap = rv().exprs.add<TR>(std::move(*op));
		op = ap;
#else
		TR* ap = obtrudeAsGreedy<T,TR>(op);
#endif
		if constexpr(std::is_same_v<T, nix::ExprLambda>){
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicExpr(expr, rv()); RF; });
		}else{
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicExpr(expr, *this); RF; });
		}
		RF;
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
		resolvedPath = nix::resolveExprPath(path);
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
	nix::Exprs& exprs = state.mem.exprs;

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

// Rewrite for when the SyntaxReference is isolated.
static void isolatedRewrite(RewriteState& state, SyntaxReference sr);
// Rewrite for when the SyntaxReference is NOT isolated.
static bool possiblyNonIsolatedRewrite(RewriteState& state, SyntaxReference sr, AnchorSet& anchors);
// Rewrite the attributes only! May or may not be isolated. This also handles the updates.
static bool rewriteMemberAttrs(RewriteState& state, SyntaxReference& sr, AnchorSet& anchors);
// We have a vague idea where we are. Descend until we find an expression that contains enough information for us to find our bearings. Returns the position of the first successfully isolated expression.
static uint32_t detachedRewrite(RewriteState& state, RawSyntaxReference rsr){
	if(auto* ptr = dynamic_cast<RecordedExprAttrs*>(rsr.expression)){
		SyntaxReference sr = rsr.descendToIsolated(ptr);
		uint32_t begin = sr.beginOffset();
		isolatedRewrite(state, std::move(sr));
		return begin;
	}else if(auto* ptr = dynamic_cast<nix::ExprLet*>(rsr.expression)){
		if(!ptr->attrs->attrs->empty()){
			SyntaxReference sr = rsr.descendToIsolated(ptr);
			uint32_t begin = sr.beginOffset();
			isolatedRewrite(state, std::move(sr));
			return begin;
		}
	}
	uint32_t acc = 0xFFFFFFFFu;
	visitDynamicExpr(rsr.expression, mkSubexprs([&](nix::Expr* ex){
		RawSyntaxReference rsr2 = rsr;
		rsr2.expression = ex;
		acc = std::min(detachedRewrite(state, rsr2), acc);
		RF;
	}));
	return acc;
}

static void isolatedRewrite(RewriteState& state, SyntaxReference sr){
	if(nix::ExprAttrs* attrs = sr.attrs()){
		AnchorSet anchors;
		if(rewriteMemberAttrs(state, sr, anchors)){
			generateNegativeRewrites(state, std::move(sr), anchors);
		}
		if(dynamic_cast<nix::ExprLet*>(sr.expression)){
			isolatedRewrite(state, sr.getBody());
		}
	}else{
		visitDynamicExpr(sr.expression, mkSubexprs([&](nix::Expr* ex){
			RawSyntaxReference rsr2 = sr;
			rsr2.expression = ex;
			detachedRewrite(state, rsr2);
			RF;
		}));
	}
}

static bool possiblyNonIsolatedRewrite(RewriteState& state, SyntaxReference sr, AnchorSet& anchors){
	uint32_t depthBeforeIsolation = sr.pathLength;
	if(sr.tryIsolate()){
		anchors.addAnchor(sr.beginOffset(), depthBeforeIsolation);
		isolatedRewrite(state, std::move(sr));
		return false;
	}else if(sr.isInherit()){
		anchors.addAnchor(sr.binarySearchPos(sr.expression->getPos())->begin, sr.pathLength);
		return false;
	}else{
		return rewriteMemberAttrs(state, sr, anchors);
	}
}

__attribute__((always_inline))
static bool rewriteMemberAttrs(RewriteState& state, SyntaxReference& sr, AnchorSet& anchors){
	nix::ExprAttrs* attrs = sr.attrs();

	EvalStateForUpdate& ext = static_cast<EvalStateForUpdate&>(state.eval());

	nix::ExprAttrs* recordedAttrs = dynamic_cast<nix::ExprAttrs*>(sr.expression);
	
	const nix::Bindings* updateBindings = nullptr;

	if(recordedAttrs){
		nix::Value& myValue = **ext.valueMap.at((RecordedExprAttrs*)recordedAttrs);

		const nix::Bindings* myBindings = myValue.attrs();

		if(const nix::Attr* updaterAttr = myBindings->get(ext.updateSymbol)){
			nix::Value& updater = *updaterAttr->value;
			ext.forceValue(updater, updaterAttr->pos);
			nix::Value* vtemp;
			if(updater.type() == nix::nFunction){
				vtemp = ext.allocValue();
				ext.callFunction(updater, myValue, *vtemp, nix::noPos);
			}else{
				vtemp = &updater;
			}
			ext.forceAttrs(*vtemp, updaterAttr->pos, "while evaluating updater");
			updateBindings = vtemp->attrs();
			generatePositiveRewrites(state, sr, *updateBindings, anchors);
		}
	}

	bool anyRewrite = updateBindings != nullptr;

	RawSyntaxReference rsr = sr;

	if(attrs->inheritFromExprs){
		for(auto& def : *attrs->inheritFromExprs.get()){
			rsr.expression = def;
			uint32_t pos = detachedRewrite(state, rsr);
			if(pos != 0xFFFFFFFFu){
				// Good enough but... aaarghhhh.
				anchors.addAnchor(pos, sr.pathLength + 1);
			}
		}
	}

	for(auto& def : attrs->attrs.value()){
		// For obvious reasons we do not descend to attributes we just overwrote.
		if(updateBindings == nullptr || updateBindings->get(def.first) == nullptr){
			anchors.addAnchor(sr.binarySearchPos(def.second.pos)->begin, sr.pathLength + 1);
			SubexpressionFrame frame;
			anyRewrite |= possiblyNonIsolatedRewrite(state, sr.getSubexpression(&frame, def.first), anchors);
		}
	}

	for(uint32_t i = 0;i < attrs->dynamicAttrs->size();i++){
		auto& dynAttr = attrs->dynamicAttrs->at(i);
		rsr.expression = dynAttr.nameExpr;
		uint32_t pos = detachedRewrite(state, rsr);
		if(pos != 0xFFFFFFFFu){
			anchors.addAnchor(pos, sr.pathLength);
		}
		
		SubexpressionFrame frame;
		possiblyNonIsolatedRewrite(state, sr.getDynamicSubexpression(&frame, i), anchors);
		anchors.addAnchor(sr.binarySearchPos(dynAttr.pos)->begin, sr.pathLength + 1);
	}

	return anyRewrite;
}

bool EvalStateForUpdate::doRewrite(std::ostream& out, const SourceInfo& info){
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

	isolatedRewrite(rewrite, std::move(root));

	std::vector<Rewrite>& rwvec = rewrite.rewrites;

	std::sort(rwvec.begin(),rwvec.end());

	uint32_t cursor = 0;

	for(Rewrite& rw : rwvec){
		out << string_view(*info.content).substr(cursor, rw.begin - cursor);
		out << rw;
		cursor = rw.end;
	}

	out << string_view(*info.content).substr(cursor);

	return !rwvec.empty();
}

}