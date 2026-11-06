
#include <dlfcn.h>
#include <exception>
#include <gc/gc_allocator.h>
#include <nix/cmd/get-build-log.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/primops.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/expr/value/context.hh>
#include <nix/store/build-result.hh>
#include <nix/store/derivations.hh>
#include <nix/store/path.hh>
#include <nix/util/error.hh>
#include <nix/util/file-system.hh>
#include <nix/util/fmt.hh>
#include <nix/util/hash.hh>
#include <nix/util/os-string.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-path.hh>
#include <nix/util/types.hh>
#include <nix/util/users.hh>
#include <nix/util/processes.hh>
#include <nix/util/util.hh>
#include <optional>

#include "steal.h"
#include "primitives.h"

EXPORT_PRIVATE_MEMBER(getFormat,&nix::HintFmt::fmt);

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

static void getErrorMessage(EvalState& state, const nix::PosIdx& pos, Value& v, Value* err){
	try{
		if(err->isFailed()){
#ifdef NDEBUG // FIXME Look in 'pins.nix'. We are developing against an older revision, but compiling against a newer one.
			std::rethrow_exception(err->failed().exRef->ex);
#else
			std::rethrow_exception(err->failed().ex);
#endif
		}else{
			state.forceValue(*err, pos);
		}
		v.mkNull();
	}catch(const nix::Error& e){
		v.mkString(e.message(), state.mem);
	}
}

static void tryEval(EvalState& state, const nix::PosIdx& pos, Value& v, Value* a){
	if(!a->isFailed()){
		nix::MaintainCount trylevel(state.trylevel);
		try{
			state.forceValue(*a, pos);
		}catch(const nix::Error&){}
	}
	v.mkBool(!a->isFailed());
}

static void getBuildLog(EvalState& state, const nix::PosIdx& pos, Value& v, Value* path){
	nix::NixStringContext context;
	nix::StorePath sp = state.coerceToStorePath(pos, *path, context, "while evaluating the argument to 'getBuildLog'");
	v.mkString(nix::fetchBuildLog(state.store, sp, sp.to_string()), state.mem);
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

	// Evaluates the argument. If it produces an error, returns false. Otherwise returns true. This catches most errors.
	builder.push_back({symbols.create("tryEval"), &primop<tryEval>});

	// Returns the error message produced by the argument as a string. Returns null if the argument is not an error.
	builder.push_back({symbols.create("getErrorMessage"), &primop<getErrorMessage>});

	// Returns the build log for the argument (which must be coercible to a drv path) as a string.
	builder.push_back({symbols.create("getBuildLog"), &primop<getBuildLog>});

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
