#pragma once

#include <gc/gc.h>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>

// Hash function and deep equality for nix values. Note that this disregards positions.

struct NixValueComparer{
	static size_t hash(nix::Value* vptr);
	static bool eq(nix::Value* l,nix::Value* r);
	inline size_t operator()(nix::Value* vptr) const{ return hash(vptr); }
	inline bool operator()(nix::Value* l,nix::Value* r) const{ return eq(l,r); }
};

nix::InternalType internalType(const nix::Value& v);

// A slightly more aggressive form of deepForce that visits every value that format might be interested in.
void deeperForce(nix::EvalState& state, nix::Value& value);

// Ensures that equal values in the argument value tree are also equal by pointer equality. This operation is expensive and not only because we take care not to produce side effects that nix code can observe.
void deduplicateSubvalues(nix::EvalState& state, nix::Value*& value);