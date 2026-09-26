
#include <compare>
#include <cstdint>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/util/pos-idx.hh>
#include <set>
#include <span>
#include <algorithm>

#include <nix/expr/nixexpr.hh>
#include <nix/util/pos-table.hh>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

#include "altparse.h"

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

void walkToToken(const NixToken *&ptr, NixToken::kind_utype type);

bool maybeWalkToClosingToken(const NixToken*& ptr);

struct AttrsDeclarationInfo{
	const NixToken *begin, *end;
};

struct InheritInfo : AttrsDeclarationInfo{
};

struct InheritFromInfo : InheritInfo{
	const NixToken *attrsBegin;
};

struct BindingInfo : AttrsDeclarationInfo{
	const NixToken* name;
	const NixToken* doteq;
	inline BindingInfo next() const{
		const NixToken* cursor = doteq + 1;
		maybeWalkToClosingToken(cursor);
		cursor++;
		return BindingInfo{
			AttrsDeclarationInfo{begin, end},
			doteq + 1,
			cursor
		};
	}
};

using AttributeDeclaration = std::variant<InheritInfo,InheritFromInfo,BindingInfo>;

struct SyntaxReference{
	const nix::PosTable::Origin& origin;
	std::span<const NixToken> boundary;
	uint32_t pathLength;
	AttrPathNode* path;
	nix::Expr* expression;
	// Find the token containing the position.
	inline const NixToken* binarySearchPos(nix::PosIdx pos){
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
	// Get the parent syntax reference for this non-isolated syntax.
	inline SyntaxReference getParent() const{
		return SyntaxReference{
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
		return path != nullptr && path->name.symbol && path->getAttrs()->attrs.value()[path->name.symbol].chooseByKind(false, true, true);
	}
	// Are we the only definition inside a dynamic attribute value?
	bool isOnlyDefinitionInDynamic() const;
	bool tryIsolate();
	nix::ExprAttrs* getAttrs();
	SyntaxReference getSubexpression(AttrPathNode* pathNode,nix::Symbol name);
	std::vector<AttributeDeclaration> findMemberDeclarationsContaining(nix::EvalState& state,const std::unordered_set<uint32_t>& positions);
	// Find every place where this non-isolated value is declared! MAY miss empty declarations.
	std::vector<AttributeDeclaration> findAllDeclarations(nix::EvalState& state) const;
};