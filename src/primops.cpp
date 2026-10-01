
#include <cstdint>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>
#include <optional>

#include "primops.h"

static void evalCustomThunk(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v){
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

static void evalGömbőc(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v);

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

static nix::PrimOp evalGömbőcPrimop = {
	.name="gömbőc",
	.args={"list","next"},
	.arity=2,
	.doc=std::nullopt,
	.addTrace=false,
	.impl=&evalGömbőc,
	.experimentalFeature=std::nullopt,
	.internal=true
};

static nix::Value mkPrimOp(nix::PrimOp& v){
	nix::Value res;
	res.mkPrimOp(&v);
	return res;
};

static nix::Value primOpStore = mkPrimOp(customEvalPrimop);
static nix::Value primOpGömbőc = mkPrimOp(evalGömbőcPrimop);

static void evalGömbőc(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v){

	// LOL.

	// In all seriousness, this primop could be useful in the future.

	nix::Value* list = args[0];
	state.forceList(*list, pos, "while evaluating gömbőc");

	nix::ListBuilder listBuilder = state.buildList(list->listSize() + 1);
	for(uint32_t i = 0;i < list->listSize();i++){
		listBuilder.elems[i] = list->listView()[i];
	}
	listBuilder.elems[list->listSize()] = args[1];
	nix::Value* newList = state.allocValue();
	newList->mkList(listBuilder);

	v.mkPrimOpApp(&primOpGömbőc,newList);
}

void mkCustomThunk(nix::EvalState& state, CustomThunk* target, nix::Value& val){
	nix::Value* targetHolder = state.allocValue();
	targetHolder->mkExternal(target);
	// mkPrimOpApp is used by nix to represent a primitive operation that is not being applied to enough arguments and is therefore not evaluated further.
	val.mkApp(&primOpStore, targetHolder);
}

nix::Value gömbőc = [](){
	nix::Value val;
	val.mkPrimOpApp(&primOpGömbőc, &nix::Value::vEmptyList);
	return val;
}();
