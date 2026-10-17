
#include <boost/callable_traits/args.hpp>
#include <boost/callable_traits/return_type.hpp>
#include <boost/type_traits/function_traits.hpp>
#include <memory>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>

#include <nix/util/logging.hh>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>
#include <type_traits>
#include <functional>
#include <gc/gc_allocator.h>
#include <nix/util/pos-idx.hh>
#include <boost/callable_traits.hpp>

#include "primitives.h"
#include "valueutil.h"
#include "exprutil.h"

namespace gpin{

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
struct ExpressionReference : HashableExternalValue{
	Env* env;
	const T* internal;
};

namespace mod{

	bool symbol_empty(const nix::AttrName& name){
		return !name.symbol;
	}

	template<typename... Modifiers>
	struct Tuple : private std::tuple<Modifiers...>{
		using TargetType = std::common_type<typename Modifiers::TargetType...>;
	private:
		template<typename T,size_t... index>
		static Value* setupHelper(std::index_sequence<index...>, EvalState& state, nix::Env& env, const T& target){
			nix::ListBuilder lb = state.buildList(sizeof...(Modifiers));
			( (lb[index] = Modifiers::setup(state, env, target)), ... );
			Value* val = state.allocValue();
			val->mkList(lb);
			return val;
		}
		template<typename T,size_t... index>
		void sanitizeHelper(std::index_sequence<index...>,EvalState& state, const T& target, nix::DebugTrace& trace) const {
			( std::get<index>(*this).sanitize(state, target, trace), ...);
		}
		template<typename T,size_t... index>
		void initHelper(std::index_sequence<index...>,EvalState& state, T& target, Value* data){
			state.forceList(*data, nix::noPos, "inside greedy modifier evaluator");
			nix::ListView list = data->listView();
			if(list.size() != sizeof...(Modifiers)){
				state.error<nix::EvalError>("bad list length for modifier evaluator. Expected %1% got %2%", sizeof...(Modifiers), list.size()).debugThrow();
			}
			( std::get<index>(*this).init(state, target, list[index]), ...);
		}
	public:
		template<typename T>
		static Value* setup(EvalState& state, nix::Env& env, const T& target){
			return setupHelper(std::index_sequence_for<Modifiers...>{}, state, env, target);
		}
		template<typename T>
		void init(EvalState& state, T& target, Value* data){ initHelper(std::index_sequence_for<Modifiers...>{}, state, target, data); }
		template<typename T>
		void sanitize(EvalState& state, const T& target, nix::DebugTrace& trace) const {
			sanitizeHelper(std::index_sequence_for<Modifiers...>{}, state, target, trace);
		}
	};

	template<typename Modifier>
	struct CastPointer : private Modifier{
	private:
		using SubType = std::remove_const_t<std::remove_pointer_t<typename Modifier::TargetType>>;
	public:
		template<typename T>
		static Value* setup(EvalState& state, nix::Env& env, const T& target){
			return Modifier::setup(state, env, static_cast<const SubType*>(target));
		}
		template<typename T>
		void init(EvalState& state, T& target, Value* data){
			const SubType* ptr = static_cast<SubType*>(target);
			Modifier::init(state, ptr, data);
			target = const_cast<SubType*>(ptr);
		}
		template<typename T>
		void sanitize(EvalState& state, const T& target, nix::DebugTrace& trace) const {
			Modifier::sanitize(state,static_cast<const SubType*>(target), trace);
		}
	};

	template<typename T,typename Modifier>
	struct CopyExpr : private Modifier{
		std::optional<T> carrier;
		using TargetType = const T*;
		static Value* setup(EvalState& state, nix::Env& env, const T* target){
			return Modifier::setup(state, env, *target);
		}
		void init(EvalState& state, const T*& target, Value* data){
			carrier.emplace(*target);
			target = &*carrier;
			Modifier::init(state, *carrier, data);
		}
		T& getCopy(){
			return *carrier;
		}
		void sanitize(EvalState& state, const T* target, nix::DebugTrace& trace) const {
			if(&trace.expr == (T*)this){
				nix::DebugTrace copy = nix::DebugTrace(std::move(trace));
				new (&trace) nix::DebugTrace{
					.pos = std::move(copy.pos),
					.expr = *target,
					.env = copy.env,
					.hint = copy.hint,
					.isError = copy.isError
				};
			}
			Modifier::sanitize(state, *target, trace);
		}
	};

	struct ThunkExpr{
		ExprValue copy;
		using TargetType = nix::Expr*;
		static Value* setup(EvalState& state, nix::Env& env, nix::Expr* target){
			if(target == nullptr){
				return &Value::vNull;
			}else{
				return target->maybeThunk(state, env);
			}
		}
		void init(EvalState& state, nix::Expr*& target, Value* data){
			copy.internal = target;
			copy.value = data;
			if(target != nullptr){
				target = &copy;
			}
		}
		void sanitize(EvalState& state, const nix::Expr* target, nix::DebugTrace& trace) const {}
	};

	template<auto handle,typename Inner = ThunkExpr>
	struct Member : private Inner{
		using TargetType = std::remove_cvref_t<std::tuple_element_t<0, boost::callable_traits::args_t<decltype(handle)>>>;
		template<typename T>
		static Value* setup(EvalState& state, nix::Env& env, const T& target){
			return Inner::setup(state, env, std::invoke(handle,target));
		}
		template<typename T>
		void init(EvalState& state, T& target, Value* data){
			Inner::init(state, std::invoke(handle, target), data);
		}
		template<typename T>
		void sanitize(EvalState& state, const T& target, nix::DebugTrace& trace) const {
			Inner::sanitize(state, std::invoke(handle, target), trace);
		}
	};

	template<auto filter, typename Inner>
	struct Filter : private Inner{
		using TargetType = Inner::TargetType;
		template<typename T>
		static Value* setup(EvalState& state, nix::Env& env, const T& target){
			if(filter(target)){
				return Inner::setup(state, env, target);
			}else{
				return &Value::vNull;
			}
		}
		template<typename T>
		void init(EvalState& state, T& target, Value* data){
			if(filter(target)){
				Inner::init(state, target, data);
			}
		}
		template<typename T>
		void sanitize(EvalState& state, const T& target, nix::DebugTrace& trace) const {
			if(filter(target)){
				Inner::sanitize(state, target, trace);
			}
		}
	};

	template<typename Inner>
	struct Span{
	private:
		using InnerTarget = Inner::TargetType;
	public:
		using TargetType = std::span<InnerTarget>;
		std::unique_ptr<Inner[]> innerMemory;
		std::unique_ptr<InnerTarget[]> replacementStorage;
		static Value* setup(EvalState& state, nix::Env& env, TargetType target){
			nix::ListBuilder lb = state.buildList(target.size());
			for(size_t i = 0;i < target.size();i++){
				lb.elems[i] = Inner::setup(state, env, target[i]);
			}
			Value* val = state.allocValue();
			val->mkList(lb);
			return val;
		}
	public:
		void init(EvalState& state, TargetType& target, Value* data){
			state.forceList(*data, nix::noPos, "inside greedy modifier evaluator");
			nix::ListView list = data->listView();
			innerMemory.reset(new Inner[list.size()]);
			replacementStorage.reset(std::allocator<InnerTarget>{}.allocate(list.size()));
			for(size_t index = 0;index < list.size();index++){
				new (replacementStorage.get() + index) InnerTarget(target[index]);
				innerMemory[index].init(state, replacementStorage[index], list[index]);
			}
			target = {replacementStorage.get(), list.size()};
		}
		template<typename T>
		void sanitize(EvalState& state, T target, nix::DebugTrace& trace) const {
			TargetType span = target;
			for(uint32_t index = 0;index < span.size();index++){
				innerMemory[index].sanitize(state, span[index], trace);
			}
		}
	};


	template<auto ptrHandle,auto extentHandle,typename Inner>
	struct ProxiedSpan : private Span<Inner>{
	private:
		using InnerTarget = Span<Inner>::TargetType;
	public:
		using TargetType = std::remove_cvref_t<std::tuple_element_t<0, boost::callable_traits::args_t<decltype(ptrHandle)>>>;
		template<typename T>
		static Value* setup(EvalState& state, nix::Env& env, const T& target){
			return Span<Inner>::setup(state, env, InnerTarget{std::invoke(ptrHandle,target),std::invoke(extentHandle,target)});
		}
	private:
		template<typename T>
		static Span<Inner> evalHelper(EvalState& state, T& target, Value* data){
			InnerTarget tt{std::invoke(ptrHandle,target),std::invoke(extentHandle,target)};
			return Span<Inner>(state, tt, data);
		}
	public:
		template<typename T>
		void init(EvalState& state, T& target, Value* data){
			decltype(auto) ptr = std::invoke(ptrHandle, target);
			decltype(auto) ext = std::invoke(extentHandle, target);
			static_assert(std::is_assignable_v<decltype(ptr)&&,typename InnerTarget::pointer>);
			static_assert(std::is_assignable_v<decltype(ext)&&,typename InnerTarget::size_type>);
			InnerTarget span = {ptr, ext};
			Span<Inner>::init(state, span, data);
			ptr = span.data();
			ext = span.size();
		}
		template<typename T>
		void sanitize(EvalState& state, const T& target, nix::DebugTrace& trace) const {
			return Span<Inner>::sanitize(state, InnerTarget{std::invoke(ptrHandle,target),std::invoke(extentHandle,target)}, trace);
		}
	};
}

template<typename ExprType,typename Modifier>
void evalModifiers(nix::EvalState& state, nix::PosIdx pos, nix::Value& v, nix::Value* expRef, nix::Value* data){
	ExpressionReference<ExprType>* eref = fromValue(state, expRef, pos, "in argument to evalSimulationThunk");

	const ExprType* tty = eref->internal;

	mod::CopyExpr<ExprType,Modifier> mod;

	mod.init(state, tty, data);

	try{
		mod.getCopy().eval(state, *eref->env, v);
	}catch(nix::EvalBaseError& e){
		std::list<nix::DebugTrace>& debugTraces = e.state.debugTraces;
		for(nix::DebugTrace& dt : e.state.debugTraces){
			mod.sanitize(state, eref->internal, dt);
		}
		throw;
	}
}

template<typename ExprType,typename Modifier>
decltype(auto) mkModifiers(EvalState& state, Env& env, const ExprType* val){
	using TName = mod::CopyExpr<ExprType,Modifier>;
	using RName = ExpressionReference<ExprType>;

	RName* t = new (gc_allocator<RName>().allocate(1)) RName();
	t->env = &env;
	t->internal = val;

	nix::Value* data = TName::setup(state, env, val);

	return allocValue(state, mkAp(&primop<evalModifiers<ExprType,Modifier>>, t, data));
};

}

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

namespace gpin{

Value* Greedy<ExprCall>::maybeThunk(EvalState& state, Env& env){
	Value* fThunk = fun->maybeThunk(state, env);
	for (size_t i = 0; i < args->size(); ++i){
		Value* n = state.allocValue();
		n->mkApp(fThunk, (*args)[i]->maybeThunk(state, env));
		fThunk = n;
	}
	return fThunk;
};

Value* Greedy<nix::ExprAssert>::maybeThunk(EvalState& state, Env& env){
	using namespace mod;
	if(dynamic_cast<nix::ExprOpEq*>(cond)){
		using TMod = Tuple<
			Member<&nix::ExprAssert::cond,CastPointer<CopyExpr<nix::ExprOpEq, Tuple<Member<&nix::ExprOpEq::e1>,Member<&nix::ExprOpEq::e2>>>>>,
			Member<&nix::ExprAssert::body>
		>;
		return mkModifiers<nix::ExprAssert, TMod>(state, env, this);
	}else{
		using TMod = Tuple<
			Member<&nix::ExprAssert::cond>,
			Member<&nix::ExprAssert::body>
		>;
		return mkModifiers<nix::ExprAssert, TMod>(state, env, this);
	}
}

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

}