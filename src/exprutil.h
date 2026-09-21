#pragma once

#include "greedy.h"
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/util/pos-table.hh>
#include <type_traits>
#include <typeindex>

using nix::Expr;

template<typename Visitor>
decltype(auto) visitDynamicExpr(Expr* expr,Visitor&& visit){
	using namespace nix;
	const std::type_info& info = typeid(*expr);

#define CASE(Name) if(auto p = dynamic_cast<Name*>(expr)){ return std::invoke(std::forward<Visitor>(visit), p); }

	CASE(ExprInt);
	CASE(ExprFloat);
	CASE(ExprString);
	CASE(ExprPath);
	CASE(ExprInheritFrom);
	CASE(ExprVar);
	CASE(ExprSelect);
	CASE(ExprOpHasAttr);
	CASE(ExprAttrs);
	CASE(ExprList);
	CASE(ExprLambda);
	CASE(ExprCall);
	CASE(ExprLet);
	CASE(ExprWith);
	CASE(ExprIf);
	CASE(ExprAssert);
	CASE(ExprOpNot);
	CASE(ExprOpEq);
	CASE(ExprOpNEq);
	CASE(ExprOpAnd);
	CASE(ExprOpImpl);
	CASE(ExprOpConcatLists);
	CASE(ExprOpUpdate);
	CASE(ExprConcatStrings);
	CASE(ExprPos);
	CASE(ExprBlackHole);
	return std::invoke(std::forward<Visitor>(visit), expr);

#undef CASE
};

#define VSUB_FUN(Name) template<typename Visitor> void visitSubexprs(nix::Name* p,Visitor& visit)

VSUB_FUN(ExprInt){}
VSUB_FUN(ExprFloat){}
VSUB_FUN(ExprString){}
VSUB_FUN(ExprPath){}
VSUB_FUN(ExprInheritFrom){}
VSUB_FUN(ExprVar){}
VSUB_FUN(ExprSelect){
	visit(p->e);
	for(auto& i : p->getAttrPath()){
		if(!i.symbol){
			visit(i.expr);
		}
	}
	if(p->def) visit(p->def);
}
VSUB_FUN(ExprOpHasAttr){
	visit(p->e);
	for(auto& i : p->attrPath){
		if(!i.symbol){
			visit(i.expr);
		}
	}
}
VSUB_FUN(ExprAttrs){
	if(p->inheritFromExprs){
		for(auto& a : *p->inheritFromExprs){
			visit(a);
		}
	}
	for(auto& a : p->attrs.value()){
		visit(a.second.e);
	}
	for(auto& a : p->dynamicAttrs.value()){
		visit(a.nameExpr);
		visit(a.valueExpr);
	}
};
VSUB_FUN(ExprList){
	for(auto& a : p->elems){
		visit(a);
	}
};
VSUB_FUN(ExprLambda){
	std::optional<nix::Formals> f = p->getFormals();
	if(f){
		for(nix::Formal& o : f->formals){
			if(o.def) visit(o.def);
		}
	}
	visit(p->body);
}
VSUB_FUN(ExprCall){
	visit(p->fun);
	for(auto& arg : p->args.value()){
		visit(arg);
	}
}
VSUB_FUN(ExprLet){
	if(p->attrs->inheritFromExprs){
		for(auto& a : *p->attrs->inheritFromExprs){
			visit(a);
		}
	}
	for(auto& a : p->attrs->attrs.value()){
		visit(a.second.e);
	}
	visit(p->body);
}
VSUB_FUN(ExprWith){
	visit(p->attrs);
	visit(p->body);
}
VSUB_FUN(ExprIf){
	visit(p->cond);
	visit(p->then);
	visit(p->else_);
}
VSUB_FUN(ExprAssert){
	visit(p->cond);
	visit(p->body);
}
VSUB_FUN(ExprOpNot){
	visit(p->e);
}
#define BINOP {visit(p->e1); visit(p->e2); }

VSUB_FUN(ExprOpEq) BINOP;
VSUB_FUN(ExprOpNEq) BINOP;
VSUB_FUN(ExprOpAnd) BINOP;
VSUB_FUN(ExprOpImpl) BINOP;
VSUB_FUN(ExprOpConcatLists) BINOP;
VSUB_FUN(ExprOpUpdate) BINOP;
VSUB_FUN(ExprConcatStrings){
	for(auto& pair : p->es){
		visit(pair.second);
	}
}
VSUB_FUN(ExprPos){}
VSUB_FUN(ExprBlackHole){}


template<typename Visitor>
struct forwardSubexprs{
	Visitor visit;
	template<typename T>
	void operator()(T* expr){
		if constexpr (!std::is_same_v<std::remove_cvref_t<T>, nix::Expr>) {
			visitSubexprs(expr,visit);
		}
	}
};

namespace{
	struct no_visit{
		template<typename...Args>
		void operator()(Args... args){}
	};
}

template decltype(auto) visitDynamicExpr<forwardSubexprs<no_visit>>(nix::Expr*, forwardSubexprs<no_visit>&&);
