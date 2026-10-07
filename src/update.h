#pragma once

#include "altparse.h"
#include "greedy.h"
#include "treewalk.h"
#include "valueutil.h"

#include <boost/container_hash/hash.hpp>
#include <boost/unordered/concurrent_flat_map_fwd.hpp>
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
#include <nix/util/source-path.hh>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpin{

using std::shared_ptr;

struct RecordedExprAttrs;

struct SourceInfo{
	ParseResult parseResult;
	std::shared_ptr<const std::string> content;
	nix::RootValue value;
	nix::SourcePath basePath;
	inline SourceInfo(nix::SourcePath bp): basePath(bp){};
};

struct EvalStateForUpdate : EvalState{

	// This pool is EXCLUSIVELY for deeperForce'd nix values. DO NOT intern a value that was not deeperForced. (See deeperForce in "valueutil.h")
	NixValueInternPool pool = NixValueInternPool(*this);

	using EvalState::EvalState;


	nix::Symbol updateSymbol;

	nix::RootValue autoArgument;

	// A list of values that must be forceValue'd.
	std::vector<nix::Value*, traceable_allocator<nix::Value*>> forceQueue;
	
	std::unordered_map<RecordedExprAttrs*, std::shared_ptr<nix::Value* const>> valueMap;
	std::unordered_map<nix::SourcePath, SourceInfo> fileSources;
	std::list<SourceInfo> customSources;
	inline virtual ~EvalStateForUpdate(){} // Ensure this is polymorphic. (EvalState isn't unfortunately!)

	const SourceInfo& loadFile(const nix::SourcePath& path);
	// The stdin argument determines whether to treat the input string as sourced from stdin.
	const SourceInfo& loadString(nix::SourcePath basePath, std::string data, bool isStdin);

	// Call this after all load operations are performed. This evaluates all loaded attribute sets.
	void finishLoad();

	bool doRewrite(std::ostream& out, const SourceInfo& sourceInfo);
};

struct RecordedExprAttrs : nix::ExprAttrs{
	RecordedExprAttrs(nix::ExprAttrs&& t): nix::ExprAttrs(std::move(t)){}
	virtual void eval(EvalState &state, Env &env, Value &v) override;
	virtual Value* maybeThunk(EvalState& state, Env& env) override;
};

}