
#include <nix/expr/eval.hh>

#include "cthunk.h"

void evalCustomThunk(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v){
	nix::Value* thunk = args[0];
	state.forceValue(*thunk, pos);
	CustomThunk* ptr;
	if(thunk->type() != nix::ValueType::nExternal || !(ptr = dynamic_cast<CustomThunk*>(thunk->external()))){
		state.error<nix::EvalError>("argument to eval custom thunk must be a custom thunk")
			.atPos(pos)
			.panic();
	}
	ptr->eval(state, v);
}

static nix::PrimOp customEvalPrimop = {
	.name="custom_evaluator",
	.args={"thunk"},
	.arity=1,
	.doc=std::nullopt,
	.addTrace=false,
	.impl=&evalCustomThunk,
	.experimentalFeature=std::nullopt,
	.internal=true
};

static nix::Value primOpStore = []() {
	nix::Value res;
	res.mkPrimOp(&customEvalPrimop);
	return res;
}();

void mkCustomThunk(nix::EvalState& state, CustomThunk* target, nix::Value& val){
	nix::Value* targetHolder = state.allocValue();
	targetHolder->mkExternal(target);
	// mkPrimOpApp is used by nix to represent a primitive operation that is not being applied to enough arguments and is therefore not evaluated further.
	val.mkApp(&primOpStore, targetHolder);
}
