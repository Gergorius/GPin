
#include <exception>
#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/primops.hh>
#include <nix/expr/value.hh>
#include <nix/expr/value/context.hh>
#include <nix/store/derivations.hh>
#include <nix/util/file-system.hh>
#include <nix/util/os-string.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-path.hh>
#include <nix/util/types.hh>
#include <nix/util/users.hh>
#include <nix/util/processes.hh>
#include <optional>
#include <unordered_map>
#include "valueutil.h"

namespace gpin{

// XXXXX This is stupid.

// The only extra builtin I need is a function that takes a FOD and builds it without complaining that the hash is wrong. Which, given what this program is for, is a perfectly logical thing to have. Right now I am in the process of figuring out what a supporting nix library should be like and what kind of builtins it needs.

using nix::EvalState;
using nix::Value;
using nix::PosIdx;

void prim_exec(const nix::Bindings& bindings, EvalState& state, const PosIdx& pos, Value& v){

	nix::Derivation d;
	nix::RunOptions options;
	nix::NixStringContext context;

	std::optional<std::string> input;

	bool programSeen = false;

	for(const auto& binding : bindings){
		const auto& symbol = state.symbols[binding.name];

		if(symbol == "program"){
			options.program = state.coerceToString(pos,*binding.value,context,"while evaluating the 'program' argument to builtins.gpin.exec",false,false).toOwned();
			programSeen = true;
		}else if(symbol == "arguments"){
			state.forceList(*binding.value, pos, "while evaluating the 'arguments' argument to builtins.gpin.exec");
			for(Value* arg : binding.value->listView()){
				options.args.push_back(
					nix::string_to_os_string(
						state.coerceToString(pos,*arg,context,"while evaluating an element of 'args'").toOwned()
					)
				);
			}
		}else if(symbol == "input"){
			options.input = state.coerceToString(pos, *binding.value, context, "while evaluating the 'input' argument to builtins.gpin.exec").toOwned();
		}else if(symbol == "workingDirectory"){
			options.chdir = state.coerceToString(pos, *binding.value, context, "while evaluating the 'workingDirectory' argument to builtins.gpin.exec").toOwned();
		}else if(symbol == "environment"){
			state.forceAttrs(*binding.value, pos, "while evaluating the 'environment' argument to builtins.gpin.exec");
			if(!options.environment){
				options.environment.emplace();
			}
			for(const auto& envBinding : *binding.value->attrs()){
				std::string s = state.coerceToString(pos, *envBinding.value, context, "while evaluating an attribute of the 'environment' argument to builtins.gpin.exec").toOwned();
				options.environment->emplace(nix::string_to_os_string(state.symbols[envBinding.name]), nix::string_to_os_string(std::move(s)));
			}
		}else{
			state.error<nix::EvalError>("unrecognised argument '%1%' to builtins.gpin.exec", symbol).atPos(pos).debugThrow();
		}
	}

	if(!programSeen){
		state.error<nix::EvalError>("'program' argument required").atPos(pos).debugThrow();
	}

	try{
		auto _ = state.realiseContext(context);
	}catch(nix::InvalidPathError& e){
		state.error<nix::EvalError>("cannot execute '%1%', since store path '%2%' is not valid", options.program.string(), e.path.to_string())
			.atPos(pos)
			.debugThrow();
	}

	auto [a, result] = nix::runProgram(std::move(options));

	nix::BindingsBuilder builder = state.buildBindings(3);
	builder.push_back(nix::Attr(
		state.symbols.create("exit"),
		coerceToValue(state, a)
	));
	builder.push_back(nix::Attr(
		state.symbols.create("output"),
		coerceToValue(state, result)
	));
	v.mkAttrs(builder);
}

struct Memoization : AbstractExternalValue{
	nix::Value* target;
	bool entered = false;
	std::unordered_map<nix::Value*, nix::Value*, NixValueComparer, LazyNixValueComparer, gc_allocator<std::pair<nix::Value* const,nix::Value*>>> cache;
	Memoization(nix::Value* t, EvalState& state): target(t), cache(0x10, NixValueComparer(), LazyNixValueComparer(state)){
	}
};

static void prim_getNixCacheDir(EvalState& state, Value& val){
	val.mkPath(state.rootPath(nix::absPath(nix::getCacheDir()).string()), state.mem);
}

static void prim_memoizedInvoke(Memoization* memo, nix::Value* arg, EvalState& state, const nix::PosIdx& pos, Value& val){
	if(memo->entered){
		state.error<nix::EvalError>("memoization was recursively entered too soon").atPos(pos).debugThrow();
	}
	memo->entered = true;
	nix::Value* memoValue;
	try{
		forceForHashing(state, *arg);
		auto [itr, inserted] = memo->cache.insert({arg, nullptr});
		if(inserted){
			itr->second = state.allocValue();
			itr->second->mkApp(memo->target, arg);
		}
		memoValue = itr->second;
		memo->entered = false;
	}catch(std::exception e){
		memo->entered = false;
		throw;
	}
	state.forceValue(*memoValue, nix::noPos);
	val = *memoValue;
}

static void prim_makeMemoize(nix::Value* function, EvalState& state, const nix::PosIdx& pos, Value& val){
	Memoization* mz = new (gc_allocator<Memoization>().allocate(1)) Memoization(function, state);
	nix::Value* mzv = state.allocValue();
	mzv->mkExternal(mz);
	val.mkPrimOpApp(&primop<&prim_memoizedInvoke>, mzv);
}

static void makePrimops(EvalState& state, const PosIdx pos, Value**, Value& v){
	nix::BindingsBuilder builder = state.buildBindings(2);

	builder.push_back(nix::Attr(
		state.symbols.create("nixCacheDir"),
		&primop<&prim_getNixCacheDir>
	));

	builder.push_back(nix::Attr(
		state.symbols.create("exec"),
		&primop<&prim_exec>
	));
	
	builder.push_back(nix::Attr(
		state.symbols.create("memoize"),
		&primop<&prim_makeMemoize>
	));

	v.mkAttrs(builder);
}

nix::RegisterPrimOp gpin(nix::PrimOp{
	.name="gpin",
	.args={},
	.arity=0,
	.doc=std::nullopt,
	.addTrace=false,
	.impl=&makePrimops,
	.experimentalFeature=std::nullopt,
	.internal=false
});

}