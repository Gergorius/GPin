
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

#include "steal.h"
#include "primitives.h"

EXPORT_PRIVATE_MEMBER(getFormat,&nix::HintFmt::fmt);

extern "C" void gpin_lib(EvalState& state, Value& v);

namespace gpin{

using nix::EvalState;
using nix::Value;

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
