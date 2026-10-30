
#include <dlfcn.h>
#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/primops.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/expr/value/context.hh>
#include <nix/store/build-result.hh>
#include <nix/store/derivations.hh>
#include <nix/util/error.hh>
#include <nix/util/file-system.hh>
#include <nix/util/hash.hh>
#include <nix/util/os-string.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-path.hh>
#include <nix/util/types.hh>
#include <nix/util/users.hh>
#include <nix/util/processes.hh>
#include <optional>
#include <regex>

#include "primitives.h"

extern "C" void gpin_lib(EvalState& state, Value& v);

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

std::regex fod_error_pattern = std::regex("hash mismatch in fixed-output derivation '(.*?)':\n  specified: (.*?)\n     got:    (.*)");
std::regex color_pattern = std::regex("\\x1b\\[35;1m(.*)\\x1b\\[0m");

static void getDerivationHash(EvalState& state, const nix::PosIdx& pos, Value& v, Value* d){

	// I spent TWO days. TWO days of my life trying to figure out a better and more reliable way to do this. As it is now, the implementation is terrible. However, this primitive enables some functionality too good not to have.

	try{
		state.realisePath(pos, *d);
	}catch(nix::Error& err){
		std::string msg = err.message();
		std::smatch matches;
		if(std::regex_match(msg,matches, fod_error_pattern)){
			std::string hash_str = matches[3].str();
			std::smatch cmatches;
			if(std::regex_match(hash_str,cmatches,color_pattern)){
				hash_str = cmatches[1].str();
			}
			nix::Hash hash = nix::Hash::parseAny(hash_str, std::nullopt);
			v.mkString(hash.to_string(nix::HashFormat::Nix32, false),state.mem);
			return;
		}else{
			err.addTrace(nix::Pos(),"'getDerivationHash' could not resolve this error to a hash");
			throw;
		}
	}
	v.mkNull();
}

static void getDllPath(EvalState& state, const nix::PosIdx& pos, Value& v, Value* dummy){
	Dl_info info;
	if(dladdr(reinterpret_cast<void*>(&gpin_lib), &info)){
		v.mkString(info.dli_fname, state.mem);
	}else{
		v.mkNull();
	}
}

static void mkLibraryImpl(nix::EvalState& state, Value& v, bool isGPinUpdate){
	nix::BindingsBuilder builder = state.buildBindings(0x10);
	nix::SymbolTable& symbols = state.symbols;
	if(state.settings.enableNativeCode){
		// I don't think we even need this.
		// builder.push_back({symbols.create("exec"), &primop<exec>});
	}

	// True if this is the "builtins.gpin" defined by gpin-update.
	builder.push_back({symbols.create("isUpdater"), state.getBool(isGPinUpdate)});

	// Attempts to realize the derivation or store path and, if it fails with a hash mismatch error, returns the observed hash. If there was no error it returns null instead.
	builder.push_back({symbols.create("getDerivationHash"), &primop<getDerivationHash>});

	// Path to the shared library which exported this.
	builder.alloc("path").mkApp(&primop<getDllPath>, &Value::vNull);

	v.mkAttrs(builder);
}

void mkLibrary(nix::EvalState& state, Value& v){
	mkLibraryImpl(state, v, true);
}

}

extern "C" void gpin_lib(EvalState& state, Value& v){
	gpin::mkLibraryImpl(state, v, false);
}
