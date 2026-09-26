
#include "greedy.h"
#include "exprutil.h"
#include "altparse.h"

#include <memory>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/main/shared.hh>
#include <nix/store/store-open.hh>
#include <nix/cmd/common-eval-args.hh>

#include <unordered_map>

using std::shared_ptr;

struct EvalStateExtensions : EvalState{
	std::map<Greedy<nix::ExprAttrs>*, nix::Value*> valueMap;
	using EvalState::EvalState;
};

struct RecordedAttrs : Greedy<nix::ExprAttrs>{
	virtual void eval(EvalState &state, Env &env, Value &v) override{
		v = *this->maybeThunk(state, env);
	}
	virtual Value* maybeThunk(EvalState& state, Env& env) override{
		Value* val = state.allocValue();
		EvalStateExtensions& ext = static_cast<EvalStateExtensions&>(state);
		ext.valueMap[this] = val;
		Greedy<nix::ExprAttrs>::eval(state, env, *val);
		return val;
	};
};

struct ExprVisitor;

struct ExprVisitor{
	void operator()(nix::Expr* expr) const{

	}
	template<typename T>
	void operator()(T* expr) const{
#define VD [&](nix::Expr* expr) -> void{ visitDynamicExpr(expr, *this); }
		visitSubexprs(expr, VD);
#undef VD
		if constexpr(std::is_same_v<T, nix::ExprAttrs>){
			
		}else{

		}
	}
};

int main(int argc, char ** argv){
	nix::initNix();
	nix::initGC();

	nix::ref<nix::Store> store = nix::openStore();

	shared_ptr<EvalState> state = std::make_shared<EvalState>(nix::LookupPath(), store, nix::fetchSettings, nix::evalSettings, store);

	nix::SourcePath file = nix::lookupFileArg(*state, argv[1]);

	ParseResult result = parseExprFromString(*state, file, state->mem.exprs);

	visitDynamicExpr(result.rootExpression, ExprVisitor{});

	result.rootExpression->bindVars(*state, state->staticBaseEnv);


}