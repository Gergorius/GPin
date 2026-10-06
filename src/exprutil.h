#pragma once

#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/util/pos-table.hh>

#include <functional>
#include <type_traits>
#include <utility>

using nix::Expr;

template<typename Visitor>
decltype(auto) visitDynamicExpr(Expr* expr,Visitor&& visit){
	using namespace nix;

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

namespace{
	template<typename Visitor>
	struct ConstVisitor{
		Visitor&& visit;
		template<typename E>
		decltype(auto) operator()(const E* arg) && {
			return std::invoke(std::forward<Visitor>(visit), arg);
		}
	};
}

template<typename Visitor>
decltype(auto) visitDynamicExpr(const Expr* expr,Visitor&& visit){
	return visitDynamicExpr(const_cast<Expr*>(expr), ConstVisitor<Visitor>{std::forward<Visitor>(visit)});
}

#define VSUB_FUN(Name) template<typename Visitor> bool visitSubexprs(nix::Name* p,Visitor&& visit)

VSUB_FUN(ExprInt){ return false; }
VSUB_FUN(ExprFloat){ return false; }
VSUB_FUN(ExprString){ return false; }
VSUB_FUN(ExprPath){ return false; }
VSUB_FUN(ExprInheritFrom){ return false; }
VSUB_FUN(ExprVar){ return false; }
VSUB_FUN(ExprSelect){
	if(visit(p->e)) return true;
	for(auto& i : p->getAttrPath()){
		if(!i.symbol){
			if(visit(i.expr)) return true;
		}
	}
	return p->def && visit(p->def);
}
VSUB_FUN(ExprOpHasAttr){
	if(visit(p->e)) return true;
	for(auto& i : p->attrPath){
		if(!i.symbol){
			if(visit(i.expr)) return true;
		}
	}
	return false;
}
VSUB_FUN(ExprAttrs){
	if(p->inheritFromExprs){
		for(auto& a : *p->inheritFromExprs){
			if(visit(a)) return true;
		}
	}
	for(auto& a : p->attrs.value()){
		if(visit(a.second.e)) return true;
	}
	for(auto& a : p->dynamicAttrs.value()){
		if(visit(a.nameExpr)) return true;
		if(visit(a.valueExpr)) return true;
	}
	return false;
};
VSUB_FUN(ExprList){
	for(auto& a : p->elems){
		if(visit(a)) return true;
	}
	return false;
};
VSUB_FUN(ExprLambda){
	std::optional<nix::Formals> f = p->getFormals();
	if(f){
		for(nix::Formal& o : f->formals){
			if(o.def && visit(o.def)) return true;
		}
	}
	return visit(p->body);
}
VSUB_FUN(ExprCall){
	if(visit(p->fun)) return true;
	for(auto& arg : p->args.value()){
		if(visit(arg)) return true;
	}
	return false;
}
VSUB_FUN(ExprLet){
	if(p->attrs->inheritFromExprs){
		for(auto& a : *p->attrs->inheritFromExprs){
			if(visit(a)) return true;
		}
	}
	for(auto& a : p->attrs->attrs.value()){
		if(visit(a.second.e)) return true;
	}
	return visit(p->body);
}
VSUB_FUN(ExprWith){
	return visit(p->attrs) || visit(p->body);
}
VSUB_FUN(ExprIf){
	return visit(p->cond) || visit(p->then) || visit(p->else_);
}
VSUB_FUN(ExprAssert){
	return visit(p->cond) || visit(p->body);
}
VSUB_FUN(ExprOpNot){
	return visit(p->e);
}
#define BINOP {return visit(p->e1) || visit(p->e2); }

VSUB_FUN(ExprOpEq) BINOP;
VSUB_FUN(ExprOpNEq) BINOP;
VSUB_FUN(ExprOpAnd) BINOP;
VSUB_FUN(ExprOpImpl) BINOP;
VSUB_FUN(ExprOpConcatLists) BINOP;
VSUB_FUN(ExprOpUpdate) BINOP;
VSUB_FUN(ExprConcatStrings){
	for(auto& pair : p->es){
		if(visit(pair.second)) return true;
	}
	return false;
}
VSUB_FUN(ExprPos){ return false; }
VSUB_FUN(ExprBlackHole){ return false; }

template<typename Visitor>
struct Subexprs{
	Visitor visit;
	template<typename T>
	bool operator()(T* expr){
		if constexpr (!std::is_same_v<std::remove_cvref_t<T>, nix::Expr>) {
			return visitSubexprs(expr,visit);
		}else{
			return false;
		}
	}
};

template<typename Visitor> decltype(auto) mkSubexprs(Visitor&& visit){
	return Subexprs<Visitor>{std::forward<Visitor>(visit)};
}

