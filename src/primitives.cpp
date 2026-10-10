
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

#include "primitives.h"

namespace gpin{

using nix::EvalState;
using nix::Value;
using nix::PosIdx;

void exec(EvalState& state, const PosIdx& pos, Value& v, Value* args){

	const nix::Bindings& bindings = fromValue(state, args, pos, "while evaluating the argument to exec");

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
		allocValue(state, a)
	));
	builder.push_back(nix::Attr(
		state.symbols.create("output"),
		allocValue(state, result)
	));
	v.mkAttrs(builder);
}

}

extern "C" void gpin_exec(EvalState& state, Value& v){
	v = gpin::primop<gpin::exec>;
}

extern "C" void gpin_getNixCacheDir(EvalState& state, Value& v){
	v.mkPath(state.rootPath(nix::absPath(nix::getCacheDir()).string()), state.mem);
}