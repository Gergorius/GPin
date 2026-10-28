#pragma once

#include <cstddef>
#include <cstdint>
#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/util/error.hh>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

#include <gc/gc.h>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>
#include <utility>

namespace gpin{

// Hash function and deep equality for nix values. Note that this disregards positions.

struct NixValueComparer{
	static size_t hash(const nix::Value* vptr);
	static bool eq(const nix::Value* l,const nix::Value* r);
	inline size_t operator()(const nix::Value* vptr) const{ return hash(vptr); }
	inline bool operator()(const nix::Value* l,const nix::Value* r) const{ return eq(l,r); }
};

struct LazyNixValueComparer{
	nix::EvalState& state;
	bool eq(nix::Value* l,nix::Value* r) const;
	inline bool operator()(nix::Value* l,nix::Value* r) const{ return eq(l,r); }
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
	// Interns the value, including all subvalues. Returns a pointer to the canonical value. If two pointers x and y are equal according to NixValueComparer then `intern(x) == intern(y)`. This method may modify the argument value in any way as long as such modification is not observable to nix code. These modifications may take place even if `intern` returns a different result. Modifying either the argument or return value of the intern pool, any subvalues, or interning them to a different pool while this pool is alive leads to undefined behavior. Note that the pool keeps alive every value it references.
	const nix::Value* intern(nix::Value* value);
};

class NixValueTopology{
	uint32_t idCounter = 1;
	std::unordered_map<const nix::Value*,uint32_t> componentIdMap;
public:
	// This function has the property that for any two compound values `X` and `Y` if there exists a path `X -> Y` then `lookupTopologyIndex(Y) <= lookupTopologyIndex(X)` otherwise `lookupTopologyIndex(X) != lookupTopologyIndex(Y)`. Additionally this value is odd for values that (possibly indirectly) reference themselves and zero for non-compound values. Results are cached.
	uint32_t lookupTopologyIndex(const nix::Value* ptr);
};

nix::InternalType internalType(const nix::Value& v);

// Forces the value until it's hash is fixed.
void forceForHashing(nix::EvalState& state, nix::Value& value);

// Ensures the value tree contains no thunks whatsoever. This is essentially a more aggressive deepForce.
void deeperForce(nix::EvalState& state, nix::Value& value);

// Visit the value. If it exists in the map then increment the corresponding value by one. Otherwise insert with a value of 1 and visit all sub-values. Returns the total number of values visited. If the template paramterer is true, leaf-like values (including thunks) are not submitted to the map.
template<bool ignoreLeaves = false>
uint32_t visitValue(const nix::Value* value, std::unordered_map<const nix::Value*,uint32_t>& seen);

// Determines the size of the value tree, except if the size is larger than the limit, the limit is returned instead. This method does no cycle detection. If the template parameter is true, attribute names also count as values.
template<bool countAttributes = false>
uint32_t visitValue(const nix::Value* value, uint32_t limit);

[[clang::always_inline]]
inline bool isCompoundValue(const nix::Value* value){
	switch(internalType(*value)){
		case nix::tAttrs:
		case nix::tListSmall:
		case nix::tListN:
		case nix::tPrimOpApp:
		case nix::tApp:
			return true;
		default:
			return false;
	}
}

[[clang::always_inline]]
inline uint32_t compoundValueSize(const nix::Value* value){
	switch(internalType(*value)){
		case nix::tAttrs:
			return value->attrs()->size();
		case nix::tListSmall:
		case nix::tListN:
			return value->listSize();
		case nix::tPrimOpApp:
		case nix::tApp:
			return 2;
		default:
			return 0;
	}
}

template<typename Visitor,typename... Args>
[[clang::always_inline]]
bool visitSubvalues(const nix::Value* value, Visitor visitor, Args&&... args){
	switch(internalType(*value)){
		case nix::tAttrs:
			for(const nix::Attr& a : *value->attrs()){
				if(visitor(a.value,args...)){
					return true;
				}
			}
			return false;
		case nix::tListSmall:
		case nix::tListN:
			for(nix::Value* const v : value->listView()){
				if(visitor(v,args...)){
					return true;
				}
			}
			return false;
		case nix::tPrimOpApp:
			return visitor(value->primOpApp().left,args...) || visitor(value->primOpApp().right,std::forward<Args>(args)...);
		case nix::tApp:
			return visitor(value->app().left,args...) || visitor(value->app().right,std::forward<Args>(args)...);
		default:
			return false;
	}
}

// Hashable external value. The hashing and equality functions in this header by default consider nix external values equal if their pointers are equal. (Due to their default operator== implementation being a bad joke.) ...unless they also derive from this class, in which case the equality operator and `getHashCode` method are used. Any class deriving from this MUST ensure operator== is reflexive, transitive, symmetric and substitutable and that equality of the hash code respects equality of objects. In all cases. We mean ALL cases.
struct HashableExternalValue : nix::ExternalValueBase{
	inline virtual std::ostream& print(std::ostream& str) const override{
		return str << "<abstract external value>";
	}
	inline virtual std::string showType() const override{
		return "<abstract external value type>";
	}
	inline virtual std::string typeOf() const override{
		return "external";
	}
	virtual bool operator==(const nix::ExternalValueBase& b) const noexcept override{
		return this == &b;
	}
	virtual uint32_t getHashCode() const noexcept{
		return std::hash<const void*>{}(this);
	}
};

}