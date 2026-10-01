
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>

#include <memory_resource>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#include <type_traits>
#include <functional>
#include <gc/gc_allocator.h>
#include <nix/util/pos-idx.hh>

#include "primops.h"
#include "exprutil.h"

using nix::Value;
using nix::ValueVector;
using nix::EvalState;
using nix::Env;

using nix::SymbolTable;
using nix::ExprSelect;
using nix::ExprCall;
using nix::ExprLet;
using nix::ExprWith;

struct ExprThunkify : public Expr{
	Expr* internal;
	ExprThunkify(Expr* ptr): internal(ptr){}
	virtual void show(const SymbolTable & symbols, std::ostream & str) const override{
		internal->show(symbols, str);
	}
	virtual void eval(EvalState &state, Env &env, Value &v) override{
		// This is illegal. Many callers of eval expect the value to not be a thunk.
		v.mkThunk(&env, internal);
	}
	virtual Value* maybeThunk(EvalState& state, Env& env) override{
		return internal->maybeThunk(state, env);
	};
	virtual nix::PosIdx getPos() const override{
		return internal->getPos();
	}
};

struct ExprValue : public Expr{
	Expr* internal;
	Value* value;
	ExprValue(){}
	ExprValue(Expr* internal0, Value* value0): internal(internal0), value(value0){}
	virtual void show(const SymbolTable & symbols, std::ostream & str) const override{
		internal->show(symbols, str);
	}
	virtual void eval(EvalState &state, Env &env, Value &v) override{
		state.forceValue(*value, internal->getPos());
		v = *value;
	}
	virtual Value* maybeThunk(EvalState& state, Env& env) override{
		return value;
	}
	virtual nix::PosIdx getPos() const override{
		return internal->getPos();
	}
};

template<typename T>
struct ExprThunk : CustomThunk{
	const T* internal;
	Env* env;
	ExprThunk(){}
	ExprThunk(const T* i, Env* e): internal(i), env(e){}
};

template<typename T,typename... Args>
T* monoNew(std::pmr::monotonic_buffer_resource& res, Args&&... args){
	return new (res.allocate(sizeof(T),alignof(T))) T(std::forward<Args>(args)...);
}

template<typename T,typename... Modifiers>
struct SimulationThunk : ExprThunk<T>{
	std::tuple<Modifiers...> mfers;
	static_assert(std::is_trivially_destructible_v<decltype(mfers)>);
	SimulationThunk(const T* internal,Env* env,std::tuple<Modifiers...> mod): ExprThunk<T>(internal,env), mfers(mod){}
private:
	template<size_t... index>
	void helper(std::index_sequence<index...>, nix::EvalState& state, nix::Value& v){
		std::tuple<typename Modifiers::template eval_state<T>...> eState;

		T copy(*this->internal);

		( std::get<index>(mfers).eval(state, copy, std::get<index>(eState)), ... );
		
		try{
			copy.eval(state, *this->env, v);
		}catch(nix::EvalBaseError& e){
			std::list<nix::DebugTrace>& debugTraces = e.state.debugTraces;
			for(auto itr = debugTraces.begin();itr != debugTraces.end();itr++){
				nix::DebugTrace& orace = *itr;
				if(&orace.expr == &copy){
					debugTraces.emplace(itr,nix::DebugTrace{
						.pos = orace.pos,
						.expr = *this->internal,
						.env = orace.env,
						.hint = orace.hint,
						.isError = orace.isError
					});
					itr++;
					itr = debugTraces.erase(itr);
				}
			}
			throw;
		}
	};

public:
	virtual void eval(nix::EvalState& state, nix::Value& v) override{
		helper(std::index_sequence_for<Modifiers...>(), state, v);
	}
};

template<typename T>
struct mem_ptr_parent{};

template<typename A,typename B>
struct mem_ptr_parent<A B::*>{
	using type = B;
};

template<auto handle>
struct ValueModifier{
	template<typename T>
	using eval_state = ExprValue;
	Value* value;
	template<typename T>
	ValueModifier(EvalState& state, Env& env, const T& base){
		Expr* eptr = std::invoke(handle,base);
		value = eptr == nullptr ? nullptr : eptr->maybeThunk(state, env);
	}
	template<typename T>
	void eval(EvalState& state, T& target, ExprValue& mod){
		if(value == nullptr) return;
		decltype(auto) exprPtr = std::invoke(handle, target);
		mod = ExprValue(exprPtr, value);
		if constexpr(std::is_reference_v<decltype(exprPtr)>){
			exprPtr = &mod;
		}else{
			std::invoke(handle, target, exprPtr);
		}
	}
};

template<typename EType,typename... Modifiers>
decltype(auto) mkModifiers(EvalState& state, Env& env, const EType* val){
	using TName = SimulationThunk<EType, Modifiers...>;

	TName* t = new (gc_allocator<TName>().allocate(1)) TName(val, &env, { Modifiers(state, env, *val)...});

	Value* v = state.allocValue();
	mkCustomThunk(state, t, *v);
	return v;
}

struct always_true{
	template<typename... Args>
	constexpr bool operator()(Args&&...) const { return true; }
};

struct symbol_empty{
	constexpr bool operator()(const nix::AttrName& name) const{
		return !name.symbol;
	}
};

struct sd_handle{
	std::span<const nix::AttrName> operator()(const ExprSelect& base) const{
		return base.getAttrPath();
	}
	void operator()(ExprSelect& base,std::span<nix::AttrName> nVal) const{
		base.attrPathStart = nVal.data();
		base.nAttrPath = nVal.size();
	}
};

template<auto handle,auto filter,typename Modifier>
struct SpanModifier{
	template<typename B>
	using range_type = std::remove_reference_t<std::invoke_result_t<decltype(handle), B&>>;

	template<typename B>
	struct eval_state{
		std::vector<typename Modifier::template eval_state<typename range_type<B>::element_type>> vec;
		std::vector<std::remove_const_t<typename range_type<B>::element_type>> rvec;
	};

	std::span<Modifier> modifiers;
	
	template<typename T>
	SpanModifier(EvalState& state, Env& env, T& base){
		decltype(auto) range = std::invoke(handle, base);
		size_t modifierCount = 0;
		for(auto& val : range){
			if(filter(val)){
				modifierCount++;
			}
		}
		Modifier* modifierArray = gc_allocator<Modifier>().allocate(modifierCount);
		size_t i = 0;
		for(auto& val : range){
			if(filter(val)){
				new (modifierArray + i) Modifier(state, env, val);
				i++;
			}
		}
		this->modifiers = { modifierArray, modifierCount };
	};

	template<typename T>
	void eval(EvalState& state, T& target, eval_state<T>& mod){
		if(modifiers.size() == 0){
			return;
		}
		decltype(auto) range = std::invoke(handle, target);

		mod.vec.resize(modifiers.size());
		mod.rvec.append_range(range);

		size_t i = 0;
		for(auto& val : mod.rvec){
			if(filter(val)){
				modifiers[i].eval(state, val, mod.vec[i]);
				i++;
			}
		}

		if constexpr(std::is_reference_v<decltype(range)>){
			range = mod.rvec;
		}else{
			std::invoke(handle, target, mod.rvec);
		}
	}
};

#define MK_RAW_GREEDY(name) \
template<> \
struct Greedy<nix::name> : nix::name{ \
	Greedy(nix::name&& t); \
	virtual Value* maybeThunk(EvalState& state, Env& env) override; \
}; \
Greedy<nix::name>::Greedy(nix::name&& t): nix::name(std::move(t)){}
#define MK_GREEDY(name, ...) \
MK_RAW_GREEDY(name) \
Value* Greedy<nix::name>::maybeThunk(EvalState& state, Env& env) { \
	return mkModifiers<nix::name, __VA_ARGS__>(state, env, this); \
}
#define MK_EVAL_GREEDY(name) \
MK_RAW_GREEDY(name) \
Value* Greedy<nix::name>::maybeThunk(EvalState& state, Env& env) { \
	Value* v = state.allocValue(); \
	nix::name::eval(state, env, *v); \
	return v; \
}

#include "greedy.h"

Value* Greedy<ExprCall>::maybeThunk(EvalState& state, Env& env){
	Value* fThunk = fun->maybeThunk(state, env);
	for (size_t i = 0; i < args->size(); ++i){
		Value* n = state.allocValue();
		n->mkApp(fThunk, (*args)[i]->maybeThunk(state, env));
		fThunk = n;
	}
	return fThunk;
};

template<typename E>
Value* maybeThunkOnBody(EvalState& state, Env& env, E& expr){
	ExprThunkify thunkMaker(expr.body);
	E copy(expr);
	copy.body = &thunkMaker;
	Value dummy;
	copy.eval(state, env, dummy);
	if(dummy.isThunk()){
		const nix::Value::ClosureThunk& thunk = dummy.thunk();
		return thunk.expr->maybeThunk(state, *thunk.env);
	}else{
		state.error<nix::EvalError>("a thunk was expected").withFrame(env, expr).atPos(expr.getPos()).panic();
		Value* copy = state.allocValue();
		*copy = dummy;
		return copy;
	}
}

Value* Greedy<ExprLet>::maybeThunk(EvalState& state, Env& env){
	return maybeThunkOnBody<ExprLet>(state, env, *this);
}
Value* Greedy<ExprWith>::maybeThunk(EvalState& state, Env& env){
	return maybeThunkOnBody<ExprWith>(state, env, *this);
}