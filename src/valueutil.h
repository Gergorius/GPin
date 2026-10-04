#pragma once

#include <unordered_set>

#include <gc/gc.h>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>

// Hash function and deep equality for nix values. Note that this disregards positions.

struct NixValueComparer{
	static size_t hash(const nix::Value* vptr);
	static bool eq(const nix::Value* l,const nix::Value* r);
	inline size_t operator()(const nix::Value* vptr) const{ return hash(vptr); }
	inline bool operator()(const nix::Value* l,const nix::Value* r) const{ return eq(l,r); }
};

// An intern pool for nix values. It is important that values submitted to the pool may not change during the pool's lifetime.
struct NixValueInternPool{

private:
	struct PoolPtr{
		mutable nix::Value* value;
		inline PoolPtr(nix::Value* p): value(p){}

		// I want to be absolutely certain unordered_set does not do any shenanigans.
		PoolPtr(const PoolPtr&) = delete;
		PoolPtr(PoolPtr&&) = delete;
		PoolPtr& operator=(const PoolPtr&) = delete;
		PoolPtr& operator=(PoolPtr&&) = delete;
	};

	struct PoolPtrHashEq{
		size_t operator()(const PoolPtr& a) const { return NixValueComparer::hash(a.value); }
		bool operator()(const PoolPtr& a,const PoolPtr& b) const { return NixValueComparer::eq(a.value,b.value); }
	};
	std::unordered_set<
		PoolPtr,
		PoolPtrHashEq,
		PoolPtrHashEq,
		traceable_allocator<PoolPtr>> pool;

	std::pair<nix::Value*,bool> recursiveIntern(nix::Value* value);

public:
	nix::EvalState& state;
	inline NixValueInternPool(nix::EvalState& s): state(s){}
	// Interns the value, including all subvalues. Returns a pointer to the canonical value. If two values x and y are equal according to NixValueComparer then intern(x) == intern(y). It is important that neither the argument nor the return value nor any sub-values reachable from them may be modified in any way for the rest of the pool's lifetime.
	const nix::Value* intern(nix::Value* value);
};

nix::InternalType internalType(const nix::Value& v);

// Ensures the value tree contains no thunks whatsoever. This is essentially a more aggressive deepForce.
void deeperForce(nix::EvalState& state, nix::Value& value);
