
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
#include <type_traits>
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
	bool operator()(nix::Expr* ex) const{
		throw nix::ParseError("Unknown expression type: %1%", typeid(*ex).name());
	}
#ifdef UPDATE_USE_RELOCATION
	nix::Exprs& exprs;
	RelocatingVisitor(nix::Exprs& e): exprs(e){}
	template<typename T>
	bool operator()(T* expr) const{
		expr = exprs.add<T>(std::move(*expr));
		visitSubexprs(expr,[&](nix::Expr* expr){ visitDynamicNixExpr<true>(expr, *this); RF; });
		RF;
	}
#else
	template<typename T>
	requires(!std::is_same_v<T, nix::Expr>)
	bool operator()(T* expr) const{ RF; }
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
	requires(!std::is_same_v<T, nix::Expr>)
	bool operator()(T* op) const{
		using TR = typename replacement<T>::type;
#ifdef UPDATE_USE_RELOCATION
		TR* ap = rv().exprs.add<TR>(std::move(*op));
		op = ap;
#else
		TR* ap = obtrudeAsGreedy<T,TR>(op);
#endif
		if constexpr(std::is_same_v<T, nix::ExprLambda>){
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicNixExpr<true>(expr, rv()); RF; });
		}else{
			visitSubexprs(ap, [&](nix::Expr* expr){ visitDynamicNixExpr<true>(expr, *this); RF; });
		}
		RF;
	}
};

nix::Value* processParseResult(EvalStateForUpdate& state, SourceInfo& info){
	visitDynamicNixExpr<true>(info.parseResult.rootExpression, ObtrusiveVisitor{state.mem.exprs});
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
// We have a vague idea where we are. Descend until we find an expression that contains enough information for us to find our bearings.
static void detachedRewrite(RewriteState& state, RawSyntaxReference rsr){
	if(auto* ptr = dynamic_cast<RecordedExprAttrs*>(rsr.expression)){
		isolatedRewrite(state, rsr.descendToIsolated(ptr));
		return;
	}else if(auto* ptr = dynamic_cast<nix::ExprLet*>(rsr.expression)){
		if(!ptr->attrs->attrs->empty()){
			isolatedRewrite(state, rsr.descendToIsolated(ptr));
			return;
		}
	}
	visitDynamicNixExpr(rsr.expression, mkSubexprs([&](nix::Expr* ex){
		detachedRewrite(state, rsr.rawWithExpression(ex));
		RF;
	}));
}

static void isolatedRewrite(RewriteState& state, SyntaxReference sr){
	if(nix::ExprAttrs* attrs = sr.attrs()){
		AnchorSet anchors;
		if(rewriteMemberAttrs(state, sr, anchors)){
			generateNegativeRewrites(state, AttrsOpenInfo::nextOpen(sr.boundary.data()), 0, anchors);
		}
		if(dynamic_cast<nix::ExprLet*>(sr.expression)){
			isolatedRewrite(state, sr.getBody());
		}
	}else{
		visitDynamicNixExpr(sr.expression, mkSubexprs([&](nix::Expr* ex){
			detachedRewrite(state, sr.rawWithExpression(ex));
			RF;
		}));
	}
}

static bool possiblyNonIsolatedRewrite(RewriteState& state, SyntaxReference sr, AnchorSet& anchors){
	if(anchors.tryIsolateAndAnchor(sr)){
		isolatedRewrite(state, std::move(sr));
		return false;
	}else if(sr.isInherit()){
		anchors.addAnchorForInheritSelector(sr);
		return false;
	}else{
		return rewriteMemberAttrs(state, sr, anchors);
	}
}

__attribute__((always_inline))
static bool rewriteMemberAttrs(RewriteState& state, SyntaxReference& sr, AnchorSet& anchors){
	nix::ExprAttrs* attrs = sr.attrs();

	anchors.tryAddAnchorForAttrs(sr);

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

	if(attrs->inheritFromExprs){
		for(uint32_t i = 0;i < attrs->inheritFromExprs->size();i++){
			std::optional<SyntaxReference> fromSr = sr.getInheritFromExpression(i);
			if(!fromSr.has_value()){
				Value temp;
				// There are no variables in the expression, period. If there were, we would be looking at a valid SyntaxReference because variables always have valid positions. I think it is safe to try evaluating it with any environment. That being said, we are forcing an attrs because this is in an inherit from selector. We are expecting this evalAttrs to throw an exception, because isolatable attribute sets also always have a position.
				state.eval().eval(attrs->inheritFromExprs->at(0), temp);
				state.eval().forceAttrs(temp, attrs->getPos(), "while trying to determine whether an inherit-from is significant");
			}
			SyntaxReference fromSyntax = *std::move(fromSr);
			anchors.addAnchorForInheritFrom(fromSyntax);
			isolatedRewrite(state, std::move(fromSyntax));
		}
	}

	for(auto& def : attrs->attrs.value()){
		// For obvious reasons we do not descend to attributes we just overwrote. The appropriate anchors have been generated by the formatter.
		if(updateBindings == nullptr || updateBindings->get(def.first) == nullptr){
			SubexpressionFrame frame;
			anyRewrite |= possiblyNonIsolatedRewrite(state, sr.getSubexpression(&frame, def.first), anchors);
		}
	}

	for(uint32_t i = 0;i < attrs->dynamicAttrs->size();i++){

		// The values of dynamic attributes are either an isolatable value or a chain of single-element attribute sets ending with an isolatable value.

		auto& dynAttr = attrs->dynamicAttrs->at(i);
		detachedRewrite(state, sr.rawWithExpression(dynAttr.nameExpr));
		SubexpressionFrame frame;
		possiblyNonIsolatedRewrite(state, sr.getDynamicSubexpression(&frame, i), anchors);
	}

	return anyRewrite;
}

bool EvalStateForUpdate::doRewrite(std::ostream& out, const SourceInfo& info){
	RewriteState rewrite{
		.pool = this->pool,
		.evalState = *this,
		.origin = *info.parseResult.origin,
		.defaultIndent = "",
		.source = *info.content,
		.basePath = info.basePath
	};

	SyntaxReference root = RawSyntaxReference{
		.origin = *info.parseResult.origin,
		.boundary = info.parseResult.tokens,
		.expression = info.parseResult.rootExpression,
	}.asIsolated();

	isolatedRewrite(rewrite, std::move(root));

	uint32_t cursor = 0;

	for(auto& pair : rewrite.replacement){
		out << string_view(*info.content).substr(cursor, pair.first.begin - cursor);
		out << pair.second.get();
		cursor = pair.first.end;
	}

	out << string_view(*info.content).substr(cursor);

	return !rewrite.replacement.empty();
}

}