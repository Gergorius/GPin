
#include "greedy.h"
#include "exprutil.h"
#include "altparse.h"
#include "format.h"

#include <memory>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/main/shared.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/cmd/common-eval-args.hh>
#include <gc/gc_allocator.h>

#include <nix/util/pos-idx.hh>
#include <nix/util/pos-table.hh>
#include <type_traits>
#include <unordered_map>

using std::shared_ptr;

struct RecordedAttrs;

struct EvalStateExtensions : EvalState{
	nix::Symbol updateSymbol;
	std::unordered_map<void*, nix::RootValue> valueMap;
	using EvalState::EvalState;
};

struct RecordedAttrs : Greedy<nix::ExprAttrs>{
	RecordedAttrs(nix::ExprAttrs&& t): Greedy<nix::ExprAttrs>(std::move(t)){}
	virtual void eval(EvalState &state, Env &env, Value &v) override{
		v = *this->maybeThunk(state, env);
	}
	virtual Value* maybeThunk(EvalState& state, Env& env) override{
		Value* val = state.allocValue();
		Greedy<nix::ExprAttrs>::eval(state, env, *val);
		static_cast<EvalStateExtensions&>(state).valueMap.emplace(this,nix::allocRootValue(val));
		return val;
	};
};

#define VD [&](nix::Expr* expr) -> void{ this->subexpr(expr); }

struct ObtrusiveVisitor{
	void subexpr(nix::Expr* e) const{
		visitDynamicExpr(e, *this);
	}
	void operator()(nix::Expr*) const{
		// TODO: We should error here.
	}
	void operator()(nix::ExprFloat*) const{}
	void operator()(nix::ExprInt*) const{}
	void operator()(nix::ExprString*) const{}
	void operator()(nix::ExprVar*) const{}
	void operator()(nix::ExprPath*) const{}
	void operator()(nix::ExprInheritFrom*) const{}
	void operator()(nix::ExprBlackHole*) const{}
	// Lambda's subexpressions are left alone.
	void operator()(nix::ExprLambda* expr) const{
		obtrudeAsGreedy<nix::ExprLambda>(expr);
	}
	void operator()(nix::ExprAttrs* expr) const{
		visitSubexprs(expr,VD);
		obtrudeAsGreedy<nix::ExprAttrs,RecordedAttrs>(expr);
	}
	template<typename T>
	void operator()(T* expr) const{
		visitSubexprs(expr, VD);
		obtrudeAsGreedy<T>(expr);
	}
};
#undef VD

namespace{
	inline SyntaxReference& descendHelper(RecordedAttrs* a, SyntaxReference& sr){
		return sr;
	}
	inline SyntaxReference descendHelper(RecordedAttrs* a, RawSyntaxReference& sr){
		return sr.descendToIsolated(a);
	}
}

template<typename SynRef>
void recursiveRewrite(RewriteState& state, SynRef sr){
	constexpr bool raw = std::is_same_v<SynRef, RawSyntaxReference>;
	if constexpr(!raw){
		sr.tryIsolate();
	}
	EvalStateExtensions& ext = static_cast<EvalStateExtensions&>(state.eval);
	if(auto a = dynamic_cast<RecordedAttrs*>(sr.expression)){

		decltype(auto) syntax = descendHelper(a, sr);

		auto itr = a->attrs->find(ext.updateSymbol);
		const nix::Bindings* myBindings = (*ext.valueMap[a])->attrs();
		const nix::Bindings* updateBindings = nullptr;
		if(itr != a->attrs->end()){
			// Must be a static attribute!

			nix::Value& updater = *myBindings->get(ext.updateSymbol)->value;

			state.eval.forceAttrs(updater, itr->second.pos, "while evaluating updater");

			updateBindings = updater.attrs();
			
			for(auto& updateBinding : *updateBindings){
				state.eval.forceValueDeep(*updateBinding.value);

				SubexpressionFrame frame;
				SyntaxReference sr = syntax.getSubexpression(&frame, updateBinding.name);
				generateRewrite(state, std::move(sr), *updateBinding.value);
			}
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
#define VT [&](nix::Expr* ex) -> void{ RawSyntaxReference rsr = sr; rsr.expression = ex; recursiveRewrite(state, rsr); }
		visitDynamicExpr(sr.expression, mkSubexprs(VT));
#undef VT
	}
}

template void recursiveRewrite<RawSyntaxReference>(RewriteState&,RawSyntaxReference);
template void recursiveRewrite<SyntaxReference>(RewriteState&,SyntaxReference);

int main(int argc, char ** argv){
	nix::initNix();
	nix::initGC();

	shared_ptr<nix::Store> store = nix::openStore();

	shared_ptr<EvalStateExtensions> state = std::make_shared<EvalStateExtensions>(nix::LookupPath(), nix::ref<nix::Store>(store), nix::fetchSettings, nix::evalSettings, store);

	state->updateSymbol = state->symbols.create("update");

	nix::SourcePath file = nix::lookupFileArg(*state, argv[1]);

	const ParseResult parseResult = parseExprFromString(*state, file, state->mem.exprs);
	visitDynamicExpr(parseResult.rootExpression, ObtrusiveVisitor{});
	parseResult.rootExpression->bindVars(*state, state->staticBaseEnv);
	parseResult.rootExpression->maybeThunk(*state, state->baseEnv);

	RewriteState rewrite{
		.eval = *state,
		.defaultIndent = "",
		.source = parseResult.sourceString,
		.sourcePath = file
	};

	SyntaxReference root = RawSyntaxReference{
		.origin = *parseResult.origin,
		.boundary = parseResult.tokens,
		.pathLength = 0,
		.path = nullptr,
		.expression = parseResult.rootExpression,
	};

	recursiveRewrite(rewrite, std::move(root));

	std::sort(rewrite.rewrites.begin(),rewrite.rewrites.end());

	std::ostream& out = std::cout;

	uint32_t cursor = 0;

	for(Rewrite& rw : rewrite.rewrites){
		out << string_view(parseResult.sourceString).substr(cursor, rw.begin - cursor);
		out << rw.replacement;
		cursor = rw.end;
	}

	out << string_view(parseResult.sourceString).substr(cursor);
	out.flush();
}