#pragma once

#include <cstdint>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/util/pos-idx.hh>
#include <span>
#include <algorithm>

#include <nix/expr/nixexpr.hh>
#include <nix/util/pos-table.hh>
#include <variant>
#include <vector>

#include "altparse.h"

namespace gpin{

struct AttrPathNode{
	AttrPathNode* parent;
	// This will either be an ExprAttrs or an ExprLet.
	nix::Expr* expr;
	nix::AttrName name; // Dynamic attributes can also be nested.
	AttrPathNode(): parent(nullptr), expr(nullptr), name(nullptr){}
	inline nix::ExprAttrs* getAttrs(){
		nix::ExprLet* let = dynamic_cast<nix::ExprLet*>(expr);
		return let == nullptr ? dynamic_cast<nix::ExprAttrs*>(expr) : let->attrs;
	}
};

union SubexpressionFrame{
	friend struct SyntaxReference;
	SubexpressionFrame(){}
private:
	char raw[sizeof(AttrPathNode)];
	AttrPathNode node;
};

// IF the pointer is looking at a token that is known to be paired with some closing token (brackets, LET/IN, WITH/';' or others) walk to the closing token. This method is meant to help with locating the end of expressions which is usually a semicolon or closing bracket that was unaccounted for.
bool maybeWalkToClosingToken(const NixToken*& ptr);

// Walk the pointer until one of the specified list of token types is encountered. Tokens within brackets are skipped!
void walkToTokens(const NixToken*& ptr,std::initializer_list<NixToken::kind_utype> types);

// Walk the pointer until one of the specified list of token types is encountered. Tokens within brackets are skipped!
#define walkToToken(ptr,...) walkToTokens(ptr,{__VA_ARGS__})

struct AttrsDeclarationInfo{
	// First token of the attribute.
	const NixToken *begin;
	// The semicolon at the end of the attribute.
	const NixToken *endsemi;
};

struct InheritInfo : AttrsDeclarationInfo{
	const NixToken* getAttrsBegin(){ return begin + 1; }
};

struct InheritFromInfo : AttrsDeclarationInfo{
	const NixToken *attrsBegin;
	const NixToken* getAttrsBegin(){ return attrsBegin; }
};

struct BindingInfo : AttrsDeclarationInfo{
	const NixToken* name;
	const NixToken* doteq;
	inline BindingInfo next() const{
		const NixToken* cursor = doteq + 1;
		walkToToken(cursor, '=', '.');
		return BindingInfo{
			AttrsDeclarationInfo{begin, endsemi},
			doteq + 1,
			cursor
		};
	}
};

using AttributeDeclaration = std::variant<InheritInfo,InheritFromInfo,BindingInfo>;

struct SyntaxReference;

// Same fields as SyntaxReference but we do not associate a lot of semantics with this one.
struct RawSyntaxReference{
	const nix::PosTable::Origin& origin;
	std::span<const NixToken> boundary;
	uint32_t pathLength;
	AttrPathNode* path;
	nix::Expr* expression;
	// Find the token containing the position.
	inline const NixToken* binarySearchPos(nix::PosIdx pos) const{
		return &*std::lower_bound(boundary.begin(),boundary.end(),NixToken(origin.offsetOf(pos)));
	}
	// Start offset of first token.
	inline uint32_t beginOffset() const{
		return boundary[0].begin;
	}
	// End offset of last token.
	inline uint32_t endOffset() const{
		return boundary[boundary.size() - 1].end;
	}
	// If this is an ExprAttrs or ExprLet, returns the attributes. Otherwise returns nullptr.
	nix::ExprAttrs* attrs() const;
	// Create a syntax reference to a descendant expression ASSUMING it is an isolated attribute set.
	SyntaxReference descendToIsolated(nix::ExprAttrs* attrs);
	// Create a syntax reference to a descendant expression ASSUMING it is a non-empty let expression.
	SyntaxReference descendToIsolated(nix::ExprLet* let);
};

/*

How do I put this into words...

SyntaxReference is used for determining where we need to insert rewrites. It tracks the location of a nix expression in the source file. The expression may or may not actually exist and it may or may not be "isolated". We just know roughly where it should be.

SyntaxReference first and foremost maintains an origin and a boundary, which is a span of tokens. And it maintains a pointer to the expression being described. The expression pointer could be null meaning it does not actually exist. Whether it exists or not, we know that the entirety of it's definition is within the boundary. The expression is said to be isolated if the boundary is precisely aligned to this definition. (Plus potential surrounding brackets!) Whenever we reference a non-isolated expression, we also maintain the attribute path with which it is reachable.

It is possible for attribute sets to not have an isolated form because they can be defined in parts. Note that identifying the parts is on a best reasonable effort basis, therefore it is possible for us to miss parts, even non-empty ones.

*/
struct SyntaxReference : RawSyntaxReference{
	SyntaxReference(RawSyntaxReference&& t): RawSyntaxReference(t){}
	SyntaxReference(const SyntaxReference&) = delete;
	SyntaxReference& operator=(const SyntaxReference&) = delete;
	SyntaxReference(SyntaxReference&&) = default;
	SyntaxReference& operator=(SyntaxReference&&) = delete;
	// Get the parent syntax reference for this non-isolated syntax.
	inline const SyntaxReference getParent() const{
		return RawSyntaxReference{
			.origin = origin,
			.boundary = boundary,
			.pathLength = pathLength - 1,
			.path = path->parent,
			.expression = path->expr
		};
	}
	// Is this an isolated syntax reference?
	inline bool isIsolated() const{
		return path == nullptr;
	}
	// Is this syntax reference an inherit selector?
	inline bool isInherit() const{
		return expression != nullptr && path != nullptr && path->name.symbol && path->getAttrs()->attrs.value().at(path->name.symbol).chooseByKind(false, true, true);
	}
	// Are we the only definition inside a dynamic attribute value?
	bool isOnlyDefinitionInDynamic() const;
	bool tryIsolate();
	// Returns the body ASSUMING this is an isolated ExprLet or ExprWith.
	SyntaxReference getBody();
	// Get a reference to a subexpression of this ExprAttrs or ExprLet which may not be dynamic.
	SyntaxReference getSubexpression(SubexpressionFrame* frame,nix::Symbol name);
	SyntaxReference getDynamicSubexpression(SubexpressionFrame* frame,uint32_t index);
	// Find all member declarations that contain at least one position from a set of positions.
	std::vector<AttributeDeclaration> findMemberDeclarationsContaining(nix::EvalState& state,const std::set<uint32_t>& positions) const;
	// Find every place where this non-isolated value is declared! MAY miss empty declarations.
	std::vector<AttributeDeclaration> findAllDeclarations(nix::EvalState& state) const;
};


template<typename Visitor>
inline bool traverseAttrsDeclarations(std::span<const NixToken> boundary,const Visitor& visit){
	uint32_t index = 0;
	const NixToken* cursor = boundary.data();
	while(cursor->type != '{' && cursor->type != NixToken::kind_type::LET){
		cursor++;
	}
	cursor++;
	while(cursor->type != '}' && cursor->type != NixToken::kind_type::IN_KW){
		const NixToken* begin = cursor;
		if(cursor->type == NixToken::kind_type::INHERIT){
			cursor++;
			if(maybeWalkToClosingToken(cursor)){ // Is this inherit-from?
				cursor++;
				const NixToken* attrsBegin = cursor;
				walkToToken(cursor, ';');
				if(visit(index,InheritFromInfo{
					AttrsDeclarationInfo{begin, cursor},
					attrsBegin,
				})){
					return true;
				}
				cursor++;
			}else{
				walkToToken(cursor, ';');
				if(visit(index,InheritInfo{
					AttrsDeclarationInfo{begin,cursor},
				})){
					return true;
				}
				cursor++;
			}
		}else{
			walkToToken(cursor, '=','.');
			const NixToken* doteq = cursor;
			walkToToken(cursor,';');
			if(visit(index,BindingInfo{
				AttrsDeclarationInfo{begin, cursor},
				begin,
				doteq
			})){
				return true;
			}
			cursor++;
		}
	}
	return false;
}

}