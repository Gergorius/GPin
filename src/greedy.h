#pragma once

/*

This header implements "greedy" nix expressions. Their maybeThunk functions produce "deep thunks" which are thunks that contain thunks for all subexpressions. If said subexpressions are also greedy, their thunks will also be deep and so on. No evaluation actually takes place, and forcing the deep thunks works as expected.

There are three main special cases:
- ExprAttrs gets evaluated, meaning calling it's maybeThunk WILL evaluate the dynamic attribute names.
- ExprLet's inner ExprAttrs is treated as a part of ExprLet rather than as a proper subexpression.
- ExprLambda does not try to make thunks of it's subexpressions. (For reasons that should be obvious!)

*/

#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <type_traits>

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

struct EvalStateWithGreedy;

using todo_call = void (EvalStateWithGreedy&);

template<typename T>
struct Greedy : T{
	Greedy() = delete;
	Greedy(const Greedy&) = delete;
};

#ifndef MK_GREEDY
#define MK_RAW_GREEDY(name) \
template<> \
struct Greedy<nix::name> : nix::name{ \
	Greedy(nix::name&& t); \
	virtual Value* maybeThunk(EvalState& state, Env& env) override; \
};
#define MK_GREEDY(name, ...) MK_RAW_GREEDY(name)
#define MK_EVAL_GREEDY(name) MK_RAW_GREEDY(name)
#else
using namespace mod;
#endif

// These expressions are fully evaluated when maybeThunk is called.

MK_EVAL_GREEDY(ExprList);
MK_EVAL_GREEDY(ExprLambda); // Lacks a maybeThunk implementation.
MK_EVAL_GREEDY(ExprPos);    // Lacks a maybeThunk implementation.

// These expressions have specialised implementations

//MK_RAW_GREEDY(ExprAttrs); // ExprAttrs has no proper implementation.
MK_RAW_GREEDY(ExprCall); // Converted to a series of app thunks.
MK_RAW_GREEDY(ExprAssert); // Uses our special thunk sauce but has a special case for equality.

// These expressions are copied with the body being replaced with a pseudo-expression that calls the maybeThunk of the actual body. The copy is then evaluatd.

MK_RAW_GREEDY(ExprLet);
MK_RAW_GREEDY(ExprWith);

// These expressions do not have nix thunks forcing us to... sigh... make a custom thunk for them. The custom thunk is a primitive operation call with a custom primitive operation and a custom external value as the first argument. Said external value is gc managed and stores a reference to the env and the original expression. When the thunk is forced, we construct a temporary copy of the expression object and replace it's subexpression pointers with temporary expressions that evaluate to the value thunks.

MK_GREEDY(ExprIf, Tuple<
	Member<&ExprIf::cond>,
	Member<&ExprIf::then>,
	Member<&ExprIf::else_>
>);
MK_GREEDY(ExprSelect, Tuple<
	Member<&ExprSelect::e>,
	ProxiedSpan<&ExprSelect::attrPathStart,&ExprSelect::nAttrPath,Filter<&symbol_empty,Member<&nix::AttrName::expr>>>,
	Member<&ExprSelect::def>
>);
MK_GREEDY(ExprOpHasAttr, Tuple<
	Member<&ExprOpHasAttr::e>,
	Member<&ExprOpHasAttr::attrPath,Span<Filter<&symbol_empty,Member<&nix::AttrName::expr>>>>
>)
MK_GREEDY(ExprOpNot, Member<&ExprOpNot::e>);
MK_GREEDY(ExprConcatStrings, Member<&ExprConcatStrings::es,Span<Member<&std::pair<nix::PosIdx,nix::Expr*>::second>>>)

#define MK_GREEDY_BINOP(name) MK_GREEDY(name,Tuple<Member<&nix::name::e1>, Member<&nix::name::e2>>)

MK_GREEDY_BINOP(ExprOpEq);
MK_GREEDY_BINOP(ExprOpNEq);
MK_GREEDY_BINOP(ExprOpAnd);
MK_GREEDY_BINOP(ExprOpOr);
MK_GREEDY_BINOP(ExprOpImpl);
MK_GREEDY_BINOP(ExprOpConcatLists);
MK_GREEDY_BINOP(ExprOpUpdate);

#undef MK_GREEDY_BINOP
#undef MK_GREEDY
#undef MK_EVAL_GREEDY
#undef MK_RAW_GREEDY

// Extremely questionably 'replaces' a nix expression with the greedy variant.
template<typename T,typename G = Greedy<T>>
G* obtrudeAsGreedy(T* arg){
	// Don't do this at home.
	static_assert(sizeof(T) == sizeof(G));
	static_assert(alignof(T) == alignof(G));
	static_assert(std::is_base_of_v<T, G>);

	T temp(std::move(*arg));
	arg->~T(); // Recall that the destructor must still be called after moving!
	G* p = new (arg) G(std::move(temp));
	return p;
}

}