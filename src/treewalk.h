#pragma once

#include <cstdint>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/util/pos-idx.hh>
#include <span>

#include <nix/expr/nixexpr.hh>
#include <nix/util/pos-table.hh>
#include <variant>
#include <vector>

#include "altparse.h"

namespace gpin{

struct AttrPathNode{
	const AttrPathNode* parent;
	// This will either be an ExprAttrs or an ExprLet.
	nix::Expr* expr;
	nix::AttrName name; // Dynamic attributes can also be nested.
	AttrPathNode(): parent(nullptr), expr(nullptr), name(nullptr){}
	inline nix::ExprAttrs* getAttrs() const{
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

// Traverse the expression tree and try to find a position.
nix::PosIdx findPosition(const nix::Expr* expression);

// Walk the pointer until one of the specified list of token types is encountered. Tokens within brackets are skipped!
#define walkToToken(ptr,...) walkToTokens(ptr,{__VA_ARGS__})

class AttrsDeclarationInfo;
class InheritInfo;
class InheritFromInfo;
class BindingInfo;
class AttrsCloseInfo;
class AttrsOpenInfo;

#define FRIENDS \
friend class AttrsDeclarationInfo;\
friend class AttrsSyntaxBase0;\
friend class AttrsSyntaxBase;\
friend class InheritInfo;\
friend class InheritFromInfo;\
friend class BindingInfo;\
friend class AttrsCloseInfo;\
friend class AttrsOpenInfo;\
friend class TreewalkFirstHolder;

class AttrsCloseInfo{
	FRIENDS;
	const NixToken* bracket;
	inline AttrsCloseInfo(const NixToken* b): bracket(b){}
public:
	inline const NixToken* getBracket(){ return bracket; }
};

class AttrsSyntaxBase0{
	FRIENDS;
	const NixToken* first;
	inline AttrsSyntaxBase0(const NixToken* f): first(f){};
public:
	inline const NixToken* getFirst() const { return first; }
};

class AttrsSyntaxBase{
	FRIENDS;
	const NixToken* last;
	inline AttrsSyntaxBase(const NixToken* l): last(l){}
public:
	// Returns the last token part of this syntax. The LAST. Not one past the last. This is typically a semicolon, but could be something different.
	inline const NixToken* getLast() const { return last; }
	std::variant<InheritInfo,InheritFromInfo,BindingInfo,AttrsCloseInfo> getNext() const;
};

class AttrsOpenInfo : public AttrsSyntaxBase{
	FRIENDS;
	AttrsOpenInfo(const NixToken* t): AttrsSyntaxBase(t){}
public:
	inline const NixToken* getOpenBracket(){ return last; }
	static AttrsOpenInfo nextOpen(const NixToken* token);
};

class AttrsDeclarationInfo : public AttrsSyntaxBase0, public AttrsSyntaxBase{
	FRIENDS;
	inline AttrsDeclarationInfo(const NixToken* b,const NixToken* e): AttrsSyntaxBase0(b), AttrsSyntaxBase(e){}
};

class InheritInfo : public AttrsDeclarationInfo{
	FRIENDS;
public:
	inline const NixToken* getAttrsBegin() const { return first + 1; }
};

class InheritFromInfo : public AttrsDeclarationInfo{
	FRIENDS;
	const NixToken *attrsBegin;
	inline InheritFromInfo(const AttrsDeclarationInfo& x, const NixToken* a): AttrsDeclarationInfo(x), attrsBegin(a){}
public:
	inline std::span<const NixToken> getFromExprBoundary() const { return {first + 1, attrsBegin };}
	inline const NixToken* getAttrsBegin() const { return attrsBegin; }
};

using AttributeDeclaration = std::variant<BindingInfo,InheritInfo,InheritFromInfo>;

class BindingInfo : public AttrsDeclarationInfo{
	FRIENDS;
	const NixToken* name;
	const NixToken* doteq;
	inline BindingInfo(const AttrsDeclarationInfo& x, const NixToken* n, const NixToken* d): AttrsDeclarationInfo(x), name(n), doteq(d){}
public:
	inline const NixToken* getNameBegin() const { return name; }
	inline const NixToken* getDoteq() const { return doteq; }
	BindingInfo getMember() const;
	AttrsOpenInfo getAttrsValue() const;
};

#undef FRIENDS

struct SyntaxReference;

// Same fields as SyntaxReference but we do not associate a lot of semantics with this one.
struct RawSyntaxReference{
	const nix::PosTable::Origin& origin;
	std::span<const NixToken> boundary;
	nix::Expr* const expression;
	inline uint32_t resolvePos(nix::PosIdx pos) const{
		return origin.offsetOf(pos);
	}
	// Find the token containing the position.
	inline const NixToken* binarySearchPos(nix::PosIdx pos) const{
		return binarySearchToken(boundary, origin.offsetOf(pos));
	}
	// Start offset of first token.
	inline uint32_t beginOffset() const{
		return boundary[0].begin;
	}
	// End offset of last token.
	inline uint32_t endOffset() const{
		return boundary[boundary.size() - 1].end;
	}
	inline RawSyntaxReference rawWithExpression(nix::Expr* expr) const{
		return RawSyntaxReference{
			.origin = origin,
			.boundary = boundary,
			.expression = expr
		};
	}
	inline SyntaxReference asIsolated();
	// If this is an ExprAttrs or ExprLet, returns the attributes. Otherwise returns nullptr.
	nix::ExprAttrs* attrs() const;
	// Create a syntax reference to a descendant expression ASSUMING it is an isolated attribute set.
	SyntaxReference descendToIsolated(nix::ExprAttrs* attrs);
	// Create a syntax reference to a descendant expression ASSUMING it is a non-empty let expression.
	SyntaxReference descendToIsolated(nix::ExprLet* let);
};

/*

How do I put this into words...

SyntaxReference is used for determining where we need to insert rewrites. It tracks the location of a nix expression in the source file.

SyntaxReference first and foremost maintains an origin and a boundary, which is a span of tokens. And it maintains a pointer to the expression being described. We know that the entirety of it's definition is within the boundary. The expression is said to be isolated if the boundary is precisely aligned to this definition. (Plus potential surrounding brackets!) Whenever we reference a non-isolated expression, we also maintain the attribute path with which it is reachable.

It is possible for attribute sets to not have an isolated form because they can be defined in parts. Note that identifying the parts is on a best reasonable effort basis, therefore it is possible for us to miss parts, even non-empty ones.

*/
struct SyntaxReference : RawSyntaxReference{
private:
	uint32_t pathLength;
	const AttrPathNode* path;
	inline SyntaxReference(const RawSyntaxReference& t, uint32_t pl, const AttrPathNode* p): RawSyntaxReference(t), pathLength(pl), path(p){}
public:
	inline explicit SyntaxReference(const RawSyntaxReference& t): RawSyntaxReference(t), pathLength(0), path(nullptr){}
	SyntaxReference(const SyntaxReference&) = delete;
	SyntaxReference& operator=(const SyntaxReference&) = delete;
	SyntaxReference(SyntaxReference&&) = default;
	SyntaxReference& operator=(SyntaxReference&&) = delete;
	inline uint32_t getPathLength() const{ return pathLength; }
	// Get the parent syntax reference for this non-isolated syntax.
	inline const SyntaxReference getParent() const{
		return SyntaxReference(RawSyntaxReference{
			.origin = origin,
			.boundary = boundary,
			.expression = path->expr
		}, pathLength - 1, path->parent);
	}
	// Returns the nix attribute name of this non-isolated SyntaxReference.
	inline const nix::AttrName& getName() const{ return path->name; }
	// Is this an isolated syntax reference?
	inline bool isIsolated() const{
		return path == nullptr;
	}
	// Is this syntax reference an inherit selector?
	inline bool isInherit() const{
		return path != nullptr && path->name.symbol && path->getAttrs()->attrs.value().at(path->name.symbol).chooseByKind(false, true, true);
	}
	// Are we the only definition inside a dynamic attribute value?
	bool isOnlyDefinitionInDynamic() const;
	bool tryIsolate();
	// Returns the body ASSUMING this is an isolated ExprLet or ExprWith.
	SyntaxReference getBody();
	// Get a reference to a subexpression of this ExprAttrs or ExprLet which may not be dynamic.
	SyntaxReference getSubexpression(SubexpressionFrame* frame,nix::Symbol name);
	SyntaxReference getDynamicSubexpression(SubexpressionFrame* frame,uint32_t index);
	// Find the inherit-from expression at the specific index.
	std::optional<SyntaxReference> getInheritFromExpression(uint32_t index);
	// Find all member declarations that contain at least one position from a set of positions.
	std::vector<AttributeDeclaration> findMemberDeclarationsContaining(const std::set<uint32_t>& positions) const;
	// Find every place where this non-isolated value is declared! MAY miss empty declarations.
	std::vector<AttributeDeclaration> findAllDeclarations() const;
};

inline SyntaxReference RawSyntaxReference::asIsolated(){ return SyntaxReference(*this); }

}