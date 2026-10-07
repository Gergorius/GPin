#pragma once

#include <cstddef>
#include <functional>
#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/util/error.hh>
#include <ostream>
#include <string_view>
#include <tuple>
#include <type_traits>
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
	// Interns the value, including all subvalues. Returns a pointer to the canonical value. If two values x and y are equal according to NixValueComparer then intern(x) == intern(y). It is important that neither the argument nor the return value nor any sub-values reachable from them may be modified in any way for the rest of the pool's lifetime.
	const nix::Value* intern(nix::Value* value);
};

nix::InternalType internalType(const nix::Value& v);

// Forces the value until it's hash is fixed.
void forceForHashing(nix::EvalState& state, nix::Value& value);

// Ensures the value tree contains no thunks whatsoever. This is essentially a more aggressive deepForce.
void deeperForce(nix::EvalState& state, nix::Value& value);

// Helper class implementing some nix external value virtual functions.
struct AbstractExternalValue : nix::ExternalValueBase{
	inline virtual std::ostream& print(std::ostream& str) const override{
		return str << "<abstract external value>";
	}
	inline virtual std::string showType() const override{
		return "<abstract external value type>";
	}
	inline virtual std::string typeOf() const override{
		return "external";
	}
	virtual bool operator==(const ExternalValueBase & b){
		return this == &b;
	}
};

template<typename T>
requires(std::is_trivially_destructible_v<T>)
struct WrappedExternalValue : AbstractExternalValue, T{
	using T::T;
};

namespace{
	template<typename T>
	struct possibly_wrap{
		using type = WrappedExternalValue<T>;
	};
	
	template<typename T>
	requires(std::is_base_of_v<nix::ExternalValueBase, T>)
	struct possibly_wrap<T>{
		using type = T;
	};
}

struct ValueConverter{
	nix::EvalState& state;
	nix::PosIdx pos;
	nix::Value* value;
	std::string_view errorCtx;
	inline operator nix::Value*() const{
		return value;
	}
	inline operator bool() const{
		return state.forceBool(*value, pos, errorCtx);
	}
	inline operator nix::NixInt() const{
		return state.forceInt(*value, pos, errorCtx);
	}
	inline operator nix::NixInt::Inner() const{
		return state.forceInt(*value, pos, errorCtx).value;
	}
	inline operator nix::NixFloat() const{
		return state.forceFloat(*value, pos, errorCtx);
	}
	inline operator std::string_view() const{
		return state.forceStringNoCtx(*value, pos, errorCtx);
	}
	inline operator const nix::Bindings*() const{
		state.forceAttrs(*value, pos, errorCtx);
		return value->attrs();
	}
	inline operator const nix::Bindings&() const{
		return *((const nix::Bindings*)(*this));
	}
	inline operator nix::ListView() const{
		state.forceList(*value, pos, errorCtx);
		return value->listView();
	}
private:
	template<typename T,size_t... idx>
	inline T tuple_helper(std::index_sequence<idx...>, nix::ListView list) const{
		return { ValueConverter(state, pos, list[idx], errorCtx) ... };
	}
public:
	template<typename... T>
	inline operator std::tuple<T...>() const{
		state.forceList(*value, pos, errorCtx);
		if(value->listSize() != sizeof...(T)){
			state.error<nix::EvalError>("list of length %1% expected", sizeof...(T)).debugThrow();
		}
		return tuple_helper<std::tuple<T...>>(std::index_sequence_for<T...>{},value->listView());
	}
	template<typename T>
	inline operator T*() const{
		using WT = possibly_wrap<T>::type;
		state.forceValue(*value, pos);
		if(value->type() != nix::nExternal){
			state.error<nix::EvalError>("external value expected").atPos(pos).debugThrow();
		}
		WT* ptr = dynamic_cast<WT*>(value->external());
		if(ptr == nullptr){
			state.error<nix::EvalError>("wrong type of external value").atPos(pos).debugThrow();
		}
		return ptr;
	}
	template<typename T>
	inline operator T&() const{
		return *((T*)(*this));
	}
};

namespace{

	template<size_t idx>
	decltype(auto) fixedConv(ValueConverter vc){
		return vc;
	}

	template<size_t idx,typename Arg0, typename... Args>
	decltype(auto) fixedConv(ValueConverter vc){
		if constexpr (idx == 0){
			return (Arg0)vc;
		}else{
			return fixedConv<idx - 1,Args...>(vc);
		}
	}

	template<typename T,typename... FixedArgTypes,size_t... indices>
	void invokeHelper(std::index_sequence<indices...>, T target, const nix::PosIdx pos, std::span<nix::Value*,sizeof...(indices)> args, nix::EvalState& state, nix::Value& v){
		if constexpr(std::is_invocable_v<decltype(target), decltype(fixedConv<indices,FixedArgTypes...>(std::declval<ValueConverter>()))..., nix::EvalState&, const nix::PosIdx&, nix::Value&>){
			std::invoke(target, fixedConv<indices,FixedArgTypes...>(ValueConverter(state, pos, args[indices], "while invoking primitive operation"))..., state, pos, v);
		}else{
			std::invoke(target, fixedConv<indices,FixedArgTypes...>(ValueConverter(state, pos, args[indices], "while invoking primitive operation"))..., state, v);
		}
	}

	template<auto target,uint32_t arity,typename... FixedArgTypes>
	void invokeWithNix(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v){
		invokeHelper<decltype(target),FixedArgTypes...>(std::make_index_sequence<arity>{}, target, pos, std::span<nix::Value*,arity>{args, arity}, state, v);
	}

	struct std_invoker{
		template<typename... Args>
		requires std::is_invocable_v<Args...>
		decltype(auto) operator()(Args&&... args){
			return std::invoke(std::forward<Args>(args)...);
		}
		template<typename Fun,typename... Args>
		requires std::is_invocable_v<Fun&,Args...>
		decltype(auto) operator()(Fun* fun, Args&&... args){
			return std::invoke(*fun, std::forward<Args>(args)...);
		}
	};

	template<typename... Args>
	constexpr uint32_t findArity(){
		if constexpr(std::is_invocable_v<Args..., nix::EvalState&, nix::Value&>){
			return sizeof...(Args) - 1;
		}else if constexpr(std::is_invocable_v<Args..., nix::EvalState&, const nix::PosIdx&, nix::Value&>){
			return sizeof...(Args) - 1;
		}else{
			static_assert(sizeof...(Args) <= nix::maxPrimOpArity);
			return findArity<Args..., ValueConverter>();
		}
	}

	inline nix::Value mkPrimOp(nix::PrimOp& v){
		nix::Value res;
		res.mkPrimOp(&v);
		return res;
	};

	template<nix::PrimOpFun target,uint32_t arity>
	inline nix::PrimOp primOpForTarget{
		.arity = arity,
		.impl = target,
		.internal = true,
	};

	template<nix::PrimOpFun target,uint32_t arity>
	inline nix::Value primopValue = mkPrimOp(primOpForTarget<target, arity>);
}

template<auto target,uint32_t arity = findArity<decltype(target)>()>
inline nix::Value& primop = primopValue<&invokeWithNix<target, arity>, arity>;

#define MK_COERCE(arg,helper) inline nix::Value* coerceToValue(nix::EvalState& state, arg){ \
	nix::Value* v = state.allocValue(); \
	helper; \
	return v; \
}

inline nix::Value* coerceToValue(nix::EvalState& state, nix::Value* value){ return value; }
MK_COERCE(nix::ExternalValueBase* e, v->mkExternal(e));
MK_COERCE(nix::NixInt i, v->mkInt(i));
MK_COERCE(nix::NixInt::Inner i, v->mkInt(i));
MK_COERCE(int i, v->mkInt(i));
MK_COERCE(nix::NixFloat f, v->mkFloat(f));
MK_COERCE(std::string_view s, v->mkString(s, state.mem));
inline nix::Value* coerceToValue(nix::EvalState& state, bool b){ return state.getBool(b); }

template<auto invoker = std_invoker{},typename... Args>
inline nix::Value* buildCustomThunk(nix::EvalState& state, Args... args){
	constexpr uint32_t arity = findArity<decltype(invoker), Args...>();

	nix::Value* v = &primopValue<&invokeWithNix<invoker, arity, Args...>, arity>;

	std::array<nix::Value*,sizeof...(Args)> argValues = { coerceToValue(state, args) ... };
	for(size_t x = 0;x < sizeof...(Args);x++){
		nix::Value* ap = state.allocValue();
		if(x + 1 < arity){
			ap->mkPrimOpApp(v, argValues[x]);
		}else{
			ap->mkApp(v, argValues[x]);
		}
		v = ap;
	}

	return v;
}

namespace{

	inline void foldInValue(nix::EvalState& state, nix::Value*& ptr, nix::ExternalValueBase* external){
		nix::Value* arg = state.allocValue();
		nix::Value* app = state.allocValue();
		arg->mkExternal(external);
		app->mkApp(ptr, arg);
		ptr = app;
	}
	
	template<typename T>
	requires(std::is_empty_v<T>)
	inline void foldInValue(nix::EvalState& state, nix::Value*& ptr, T val){
		// No fold.
	}

	template<typename T>
	requires(std::is_same_v<T, nix::ExternalValueBase*>)
	inline nix::ExternalValueBase* foldOutValue(nix::EvalState& state, nix::PosIdx& pos, nix::Value**& ptr){
		state.forceValue(**ptr, pos);
		nix::ExternalValueBase* e = (**ptr).external();
		ptr++;
		return e;
	}

	template<typename T>
	struct ValueProcessor{};

	template<>
	struct ValueProcessor<bool>{
		bool foldOut(nix::EvalState& state,const nix::PosIdx& pos, nix::Value**& ptr){
			bool r = state.forceBool(**ptr, pos, "in");
			ptr++;
			return r;
		}
	};

	template<typename... T>
	struct type_list{};

	template<size_t index,typename... Processors,typename... Args>
	inline void doInvoke(std::tuple<Processors...> proc,nix::EvalState& state,const nix::PosIdx& pos,nix::Value**& ptr,nix::Value& val,Args&&... args){
		if constexpr(index == sizeof...(Processors)){
			std::invoke(std::forward<Args>(args)...,state,pos,val);
		}else{
			doInvoke<index + 1>(proc,state,pos,ptr,val,std::forward<Args>(args)...,std::get<index>(proc).foldOut(state, pos, ptr));
		}
	}

	template<typename... Processors>
	inline void invokePrimop(nix::EvalState& state,const nix::PosIdx& pos,nix::Value** ptr,nix::Value val){
		doInvoke<0>(std::make_tuple(Processors()...), state, pos, ptr, val);
	}

	template<typename... Args>
	inline nix::Value* buildCustomThunk0(nix::EvalState& state, Args... args){
		nix::Value* acc = state.allocValue();

		(ValueProcessor<Args>::foldInValue(state, acc, args), ... );

		
	}

}


}