#include <gc/gc_allocator.h>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/value.hh>
#include <nix/expr/eval.hh>
#include <nix/util/error.hh>
#include <nix/util/pos-idx.hh>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

using nix::Value;
using nix::EvalState;

namespace gpin{

inline void toValue(EvalState& state, Value& v, nix::ExternalValueBase* e){ v.mkExternal(e); }
inline void toValue(EvalState& state, Value& v, nix::NixInt value){ v.mkInt(value); }
inline void toValue(EvalState& state, Value& v, std::string_view s){ v.mkString(s,state.mem); }
inline void toValue(EvalState& state, Value& v, nix::NixFloat f){ v.mkFloat(f); }
inline void toValue(EvalState& state, Value& v, nix::Value) = delete;

template<typename T>
inline Value* allocValue(EvalState& state, T&& v){
	Value* ptr = state.allocValue();
	toValue(state, *ptr, std::forward<T>(v));
	return ptr;
}
inline Value* allocValue(EvalState& state, Value* v){ return v; }

namespace{
	template<typename L,typename R>
	struct ap{
		L left;
		R right;
	};
}

template<typename L,typename R>
inline void toValue(EvalState& state, Value& v, ap<L,R> a){
	v.mkApp(allocValue(state, a.left), allocValue(state, a.right));
}

template<typename... Args,typename ArgN>
decltype(auto) mkAp(Args&&... args,ArgN&& argn){
	if constexpr(sizeof...(Args) == 0){
		return std::forward<ArgN>(argn);
	}else{
		return mkApObj(mkAp(std::forward<Args>(args)...), std::forward<ArgN>(argn));
	}
}

template<typename L,typename R>
inline ap<L,R> mkAp(L&& l,R&& r){
	return {std::forward<L>(l),std::forward<R>(r)};
}

template<typename Arg0,typename Arg1,typename... Args>
requires(sizeof...(Args) != 0)
decltype(auto) mkAp(Arg0&& arg0,Arg1&& arg1,Args&&... rest){
	return mkAp(mkAp(std::forward<Arg0>(arg0), std::forward<Arg1>(arg1)), std::forward<Args>(rest)...);
}

template<typename T>
struct from_nix_value{};

template<>
struct from_nix_value<bool>{
	bool operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return state.forceBool(*v, pos, context);
	}
};

template<>
struct from_nix_value<std::string_view>{
	std::string_view operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return state.forceStringNoCtx(*v, pos, context);
	}
};

template<>
struct from_nix_value<const nix::Bindings&>{
	const nix::Bindings& operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		state.forceAttrs(*v, pos, context);
		return *v->attrs();
	}
};

template<typename T>
requires(std::is_base_of_v<nix::ExternalValueBase, T>)
struct from_nix_value<T*>{
	T* operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		state.forceValue(*v, pos);
		T* p = nullptr;
		if(v->type() == nix::nExternal && (p = dynamic_cast<T*>(v->external()))){
			return p;
		}else{
			state.error<nix::EvalError>("external value expected").atPos(pos).debugThrow();
		}
	}
};

template<typename T>
requires(std::is_integral_v<T>)
struct from_nix_value<T>{
	T* operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return state.forceInt(*v, pos, context).value;
	}
};

template<>
struct from_nix_value<nix::NixFloat>{
	nix::NixFloat operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return state.forceFloat(*v, pos, context);
	}
};

template<typename Combiner,typename... TConv>
struct nix_list_to_fixed{
	Combiner combiner;
	std::tuple<TConv...> converters;
private:
	template<size_t... idx>
	decltype(auto) helper(std::index_sequence<idx...>,EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		state.forceList(*v, pos, context);
		if(v->listSize() != sizeof...(TConv)){
			state.error<nix::EvalError>("list of length %1% expected but found %2%", sizeof...(TConv), v->listSize()).withTrace(pos, context).debugThrow();
		}
		nix::ListView view = v->listView();
		try{
			return combiner(std::get<idx>(converters)(state, view[idx], pos, context)...);
		}catch(nix::Error& e){
			e.addTrace(state.positions[pos], context);
			throw;
		}
	}
public:
	decltype(auto) operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return helper(std::index_sequence_for<TConv...>{}, state, v, pos, context);
	}
};

namespace{
	struct tuple_maker{
		template<typename... Args>
		decltype(auto) operator()(Args&&... args) const{
			return std::make_tuple(std::forward<Args>(args)...);
		}
	};
}

template<typename... Types>
struct from_nix_value<std::tuple<Types...>> : nix_list_to_fixed<tuple_maker, from_nix_value<Types>...>{};

struct ImplicitConverter{
	EvalState& state;
	Value* value;
	nix::PosIdx pos;
	std::string_view context;
	template<typename T>
	requires (requires(const from_nix_value<T> fnv,EvalState& s,Value* v,nix::PosIdx p,std::string_view c){ fnv(s,v,p,c); })
	inline operator T() const{
		return from_nix_value<T>{}(state, value, pos, context);
	}
	template<typename T>
	requires (requires(const from_nix_value<T&> fnv,EvalState& s,Value* v,nix::PosIdx p,std::string_view c){ fnv(s,v,p,c); })
	inline operator T&() const{
		return from_nix_value<T&>{}(state, value, pos, context);
	}
};

template<>
struct from_nix_value<ImplicitConverter>{
	ImplicitConverter operator()(EvalState& state, Value* v, nix::PosIdx pos, std::string_view context) const{
		return ImplicitConverter{state, v, pos, context};
	}
};

template<typename T = ImplicitConverter>
inline decltype(auto) fromValue(EvalState& s, Value* v, nix::PosIdx pos, std::string_view trace){
	return from_nix_value<T>{}(s,v,pos,trace);
}

template<typename T = ImplicitConverter>
inline decltype(auto) fromValue(EvalState& s, Value* v, nix::PosIdx pos){
	return fromValue<T>(s, v, pos, std::string_view{});
}

template<typename T = ImplicitConverter>
inline decltype(auto) fromValue(EvalState& s, Value* v){
	return fromValue<T>(s, v, nix::noPos);
}

namespace{

	template<uint32_t arity,typename... Args>
	decltype(auto) invokeHelper(nix::Value** v, Args&&... args){
		if constexpr(arity == 0){
			return std::invoke(std::forward<Args>(args)...);
		}else{
			return invokeHelper<arity - 1>(v + 1, std::forward<Args>(args)..., *v);
		}
	}

	template<auto target,std::pair<uint32_t,bool> arityPosInfo>
	void invokeWithNix(nix::EvalState& state, const nix::PosIdx pos, nix::Value** args, nix::Value& v){
		if constexpr(arityPosInfo.second){
			invokeHelper<arityPosInfo.first>(args, target, state,pos, v);
		}else{
			invokeHelper<arityPosInfo.first>(args, target, state, v);
		}
	}

	template<typename Fun,typename... Args>
	constexpr std::pair<uint32_t,bool> findArity(){
		if constexpr(std::is_invocable_v<Fun, nix::EvalState&, const nix::PosIdx&, nix::Value&, Args...>){
			return {sizeof...(Args), true};
		}else if constexpr(std::is_invocable_v<Fun, nix::EvalState&, nix::Value&, Args...>){
			return {sizeof...(Args), false};
		}else{
			static_assert(sizeof...(Args) <= nix::maxPrimOpArity);
			return findArity<Fun, Args..., nix::Value*>();
		}
	}

	inline nix::Value mkPrimOp(nix::PrimOp& v){
		nix::Value res;
		res.mkPrimOp(&v);
		return res;
	};

	template<nix::PrimOpFun target,uint32_t arity>
	inline nix::PrimOp primOpForTarget{
		.name = "gpin_primop",
		.arity = arity,
		.impl = target,
		.internal = true,
	};

	template<nix::PrimOpFun target,uint32_t arity>
	inline nix::Value primopValue = mkPrimOp(primOpForTarget<target, arity>);
}

template<auto target>
inline nix::Value& primop = primopValue<&invokeWithNix<target, findArity<decltype(target)>()>, findArity<decltype(target)>().first>;

}