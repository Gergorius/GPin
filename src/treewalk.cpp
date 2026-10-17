
#include "treewalk.h"
#include "altparse.h"
#include "exprutil.h"
#include <nix/expr/nixexpr.hh>
#include <nix/expr/value.hh>
#include <nix/util/error.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/util.hh>
#include <type_traits>
#include <variant>
#include <vector>

namespace gpin{

template<typename Visitor>
inline bool visitAttributeDefinitions(const nix::ExprAttrs* attrs,const Visitor& visit){
	for(auto& pair : attrs->attrs.value()){
		if(visit(pair.second)){
			return true;
		}
	}
	for(auto& pair : attrs->dynamicAttrs.value()){
		if(visit(pair)){
			return true;
		}
	}
	return false;
}

void walkToTokens(const NixToken*& ptr,std::initializer_list<NixToken::kind_utype> types){
	while(true){
		for(auto type : types){
			if(ptr->type == type){
				return;
			}
		}
		maybeWalkToClosingToken(ptr);
		ptr++;
	}
}

bool maybeWalkToClosingToken(const NixToken*& ptr){
	switch(ptr->type){
		case '\"':
			walkToToken(++ptr, '\"');
			return true;
		case NixToken::kind_type::IND_STRING_OPEN:
			walkToToken(++ptr, NixToken::kind_type::IND_STRING_CLOSE);
			return true;
		case NixToken::kind_type::DOLLAR_CURLY:
		case '{':
		case_open_curly:
			walkToToken(++ptr, '}');
			return true;
		case '(':
			walkToToken(++ptr, ')');
			return true;
		case '[':
			walkToToken(++ptr, ']');
			return true;
		case NixToken::kind_type::LET:
			if(ptr[1].type == '{'){
				ptr++;
				goto case_open_curly;
			}
			walkToToken(++ptr, NixToken::kind_type::IN_KW);
			return true;
		case NixToken::kind_type::WITH:
		case NixToken::kind_type::ASSERT:
			walkToToken(++ptr, ';');
			return true;
	}
	return false;
}

namespace{
	struct PositionFinder{
		nix::PosIdx pos = nix::noPos;
		bool operator()(const nix::ExprLet* exprLet){
			// Otherwise we miss the position stored in the attrs.
			return visitSubexprs(exprLet->attrs,[this](nix::Expr* e){ return visitDynamicNixExpr(e, *this); })
			    || visitDynamicNixExpr(exprLet->body, *this);
		}
		bool operator()(const nix::ExprAttrs* expr){
			pos = expr->getPos();
			if(pos != nix::noPos){
				return true;
			}
			for(auto& binding : expr->attrs.value()){
				pos = binding.second.pos;
				if(pos != nix::noPos){
					return true;
				}
			}
			for(auto& binding : expr->dynamicAttrs.value()){
				pos = binding.pos;
				if(pos != nix::noPos){
					return true;
				}
			}
			return visitSubexprs(expr,[this](const nix::Expr* e){ return visitDynamicNixExpr(e, *this); });
		}
		template<typename T>
		bool operator()(const T* expr){
			pos = expr->getPos();
			if(pos != nix::noPos){
				return true;
			}
			if constexpr(std::is_same_v<nix::Expr, std::remove_cvref_t<T>>){
				return false;
			}else{
				return visitSubexprs(expr,[this](const nix::Expr* e){ return visitDynamicNixExpr(e, *this); });
			}
		}
	};
}

nix::PosIdx findPosition(const nix::Expr* expression){
	PositionFinder finder;
	visitDynamicNixExpr(expression, finder);
	return finder.pos;
}

AttrsOpenInfo AttrsOpenInfo::nextOpen(const NixToken* token){
	while(token->type != '{' && token->type != NixToken::kind_type::LET){
		token++;
	}
	return AttrsOpenInfo(token);
}

std::variant<InheritInfo,InheritFromInfo,BindingInfo,AttrsCloseInfo> AttrsSyntaxBase::getNext() const{
	const NixToken* cursor = last + 1;
	if(cursor->type == '}' || cursor->type == NixToken::kind_type::IN_KW){
		return AttrsCloseInfo(cursor);
	}
	const NixToken* begin = cursor;
	if(cursor->type == NixToken::kind_type::INHERIT){
		cursor++;
		if(maybeWalkToClosingToken(cursor)){ // Is this inherit-from?
			cursor++;
			const NixToken* attrsBegin = cursor;
			walkToToken(cursor, ';');
			return InheritFromInfo{
				AttrsDeclarationInfo{begin, cursor},
				attrsBegin,
			};
		}else{
			walkToToken(cursor, ';');
			return InheritInfo{AttrsDeclarationInfo{begin,cursor}};
		}
	}else{
		walkToToken(cursor, '=','.');
		const NixToken* doteq = cursor;
		walkToToken(cursor,';');
		return BindingInfo{
			AttrsDeclarationInfo{begin, cursor},
			begin,
			doteq
		};
	}
}

BindingInfo BindingInfo::getMember() const{
	const NixToken* cursor = doteq + 1;
	walkToToken(cursor, '=', '.');
	return BindingInfo(
		AttrsDeclarationInfo{*this},
		doteq + 1,
		cursor
	);
}

AttrsOpenInfo BindingInfo::getAttrsValue() const{
	return AttrsOpenInfo::nextOpen(doteq + 1);
}

struct CollectPositions{
	const nix::PosTable::Origin& origin;
	std::set<uint32_t>& positions;
	bool operator()(const nix::ExprAttrs::AttrDef& def) const{
		positions.insert(origin.offsetOf(def.pos));
		if(def.chooseByKind(true, false, false)){
			const nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(def.e);
			if(attrs){
				visitAttributeDefinitions(attrs, *this);
			}
		}
		return false;
	}
	bool operator()(const nix::ExprAttrs::DynamicAttrDef& def) const{
		positions.insert(origin.offsetOf(def.pos));
		return false;
	}
};

bool SyntaxReference::isOnlyDefinitionInDynamic() const{
	const AttrPathNode* node = path;
	while(node != nullptr){
		if(node->name.symbol){
			if(node->getAttrs()->pos != nix::noPos){
				return false;
			}
		}else{
			return true;
		}
		node = node->parent;
	}
	return false;
}

bool SyntaxReference::tryIsolate(){
	if(path == nullptr){
		return true;
	}
	nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(this->expression);
	if(attrs == nullptr || attrs->pos != nix::noPos && isOnlyDefinitionInDynamic()){
		nix::PosIdx pos;
		nix::ExprAttrs* parent = path->getAttrs();

		if(path->name.symbol){
			nix::ExprAttrs::AttrDef& def = parent->attrs.value()[path->name.symbol];
			if(def.chooseByKind(false, true, true)){
				return false; // An inherit cannot be isolated.
			}
			pos = def.pos;
		}else{
			for(auto& dynAttr : parent->dynamicAttrs.value()){
				if(dynAttr.valueExpr == this->expression){
					pos = dynAttr.pos;
					goto doTraverse;
				}
			}
			// Error: Not found. This should not be possible.
		}
	doTraverse:
		const NixToken* cursor = binarySearchPos(pos);
		walkToToken(cursor, '=');
		cursor++;
		const NixToken* begin = cursor;
		walkToToken(cursor, ';');
		pathLength = 0;
		path = nullptr;
		boundary = { begin, cursor};
		return true;
	}
	return false;
}

SyntaxReference RawSyntaxReference::descendToIsolated(nix::ExprAttrs* attrs){
	const NixToken* cursor = binarySearchPos(attrs->getPos());
	while(cursor->type != '{' && cursor->type != NixToken::kind_type::REC){
		cursor++;
	}
	const NixToken* begin = cursor;
	while(cursor->type != '{'){
		cursor++;
	}
	maybeWalkToClosingToken(cursor);
	cursor++;
	return RawSyntaxReference{
		.origin = origin,
		.boundary = {begin, cursor},
		.expression = attrs,
	}.asIsolated();
}

SyntaxReference RawSyntaxReference::descendToIsolated(nix::ExprLet* let){
	const NixToken* cursor = boundary.data();

	const NixToken* bestTokLet;
	const NixToken* bestTokIn;

	while(cursor != boundary.data() + boundary.size()){
		if(cursor->type == NixToken::kind_type::LET){
			if(cursor[1].type == '{'){
				goto continueOuter;
			}
			const NixToken* tokLet = cursor;
			const NixToken* tokIn = cursor;
			walkToToken(tokIn, NixToken::kind_type::IN_KW);

			nix::PosIdx pos = nix::noPos;
			for(auto& def : let->attrs->attrs.value()){
				uint32_t off = origin.offsetOf(def.second.pos);
				if(off < tokLet->begin){
					goto breakOuter;
				}
				if(tokIn->end < off){
					goto continueOuter;
				}
			}
			bestTokLet = tokLet;
			bestTokIn = tokIn;
		}
	continueOuter:
		cursor++;
	}
breakOuter:
	const NixToken* end = bestTokIn;
	while(end != boundary.data() + boundary.size() && end->type != ')' && end->type != ']' && end->type != '}' && end->type != ';'){
		maybeWalkToClosingToken(end);
		end++;
	}
	return RawSyntaxReference{
		.origin = origin,
		.boundary = {bestTokIn, end},
		.expression = let,
	}.asIsolated();
}

nix::ExprAttrs* RawSyntaxReference::attrs() const{
	nix::ExprLet* let = dynamic_cast<nix::ExprLet*>(expression);
	return let == nullptr ? dynamic_cast<nix::ExprAttrs*>(expression) : let->attrs;
}

SyntaxReference SyntaxReference::getBody(){
	nix::ExprLet* let = dynamic_cast<nix::ExprLet*>(expression);
	if(let){
		const NixToken* cursor = boundary.data();
		while(cursor->type != NixToken::kind_type::LET){
			cursor++;
		}
		cursor++;
		walkToToken(cursor, NixToken::kind_type::IN_KW);
		cursor++;
		return RawSyntaxReference{
			.origin = origin,
			.boundary = {cursor, boundary.data() + boundary.size()},
			.expression = let->body,
		}.asIsolated();
	}else{
		nix::ExprWith* with = static_cast<nix::ExprWith*>(expression);
		const NixToken* cursor = boundary.data();
		while(cursor->type != NixToken::kind_type::WITH){
			cursor++;
		}
		cursor++;
		walkToToken(cursor, ';');
		cursor++;
		return RawSyntaxReference{
			.origin = origin,
			.boundary = {cursor, boundary.data() + boundary.size()},
			.expression = with->body
		}.asIsolated();
	}
}

SyntaxReference SyntaxReference::getSubexpression(SubexpressionFrame* frame,nix::Symbol name){
	AttrPathNode* pathNode = &frame->node;
	pathNode->parent = this->path;
	pathNode->expr = this->expression;
	pathNode->name = name;
	nix::ExprAttrs* expr = pathNode->getAttrs();
	nix::Expr* sub = expr->attrs->at(name).e;
	return SyntaxReference(
		RawSyntaxReference{
			.origin = origin,
			.boundary = boundary,
			.expression = sub
		},
		pathLength + 1,
		pathNode
	);
}

SyntaxReference SyntaxReference::getDynamicSubexpression(SubexpressionFrame* frame,uint32_t index){
	AttrPathNode* pathNode = &frame->node;
	nix::ExprAttrs* expr = dynamic_cast<nix::ExprAttrs*>(expression);
	auto& se = expr->dynamicAttrs->at(index);
	pathNode->parent = this->path;
	pathNode->expr = this->expression;
	pathNode->name = se.nameExpr;
	return SyntaxReference(
		RawSyntaxReference{
			.origin = origin,
			.boundary = boundary,
			.expression = se.valueExpr
		},
		pathLength + 1,
		pathNode
	);
}

std::optional<SyntaxReference> SyntaxReference::getInheritFromExpression(uint32_t index){
	nix::ExprAttrs* expr = attrs();
	nix::Expr* fromExpr = expr->inheritFromExprs->at(index);
	nix::PosIdx pos = findPosition(fromExpr);
	if(pos == nix::noPos){
		// This is a last resort!
		for(auto& binding : *expr->attrs){
			if(binding.second.chooseByKind(false, false, true)){
				nix::ExprSelect* select = dynamic_cast<nix::ExprSelect*>(binding.second.e);
				nix::ExprInheritFrom* ifrom = select == nullptr ? nullptr : dynamic_cast<nix::ExprInheritFrom*>(select->e);
				if(ifrom != nullptr && ifrom->displ == index){
					pos = ifrom->pos;
					break;
				}
			}
		}
	}
	if(pos == nix::noPos){
		// I find it unlikely that this expression, which contains no attribute sets, no variables, no operators and no selectors evaluates to bindings. At least we can be certain that this is a constant expression whose value is unaffected by updating. However, this does pose a problem: We cannot identify the declaration containing the expression. Not even if we go through every single declaration individually. And therefore we cannot anchor it.
		return std::nullopt;
	}
	uint32_t resolvedPosition = resolvePos(pos);
	std::vector<AttributeDeclaration> dec = findMemberDeclarationsContaining({resolvedPosition});
	InheritFromInfo& info = std::get<InheritFromInfo>(dec.at(0));
	return SyntaxReference(
		RawSyntaxReference{
			.origin = origin,
			.boundary = info.getFromExprBoundary(),
			.expression = fromExpr
		},
		0,
		nullptr
	);
}

std::vector<AttributeDeclaration> SyntaxReference::findMemberDeclarationsContaining(const std::set<uint32_t>& positions) const{
	if(isIsolated()){
		std::vector<AttributeDeclaration> vec;
		AttrsSyntaxBase base = AttrsOpenInfo::nextOpen(boundary.data());
		bool end = false;
		while(!end){
			std::visit(nix::overloaded{
				[&](const AttrsCloseInfo& p) {
					end = true;
				},
				[&](const auto& b) {
					auto itr = positions.lower_bound(b.getFirst()->begin);
					if(itr != positions.end() && *itr < b.getLast()->end){
						vec.push_back(b);
					}
					base = b;
				},
			}, base.getNext());
		}
		return vec;
	}else{
		std::vector<AttributeDeclaration> pvec = getParent().findMemberDeclarationsContaining(positions);
		std::vector<AttributeDeclaration> vec;
		for(AttributeDeclaration d : pvec){
			if(std::holds_alternative<BindingInfo>(d)){
				const BindingInfo& info = std::get<BindingInfo>(d);
				switch(info.getDoteq()->type){
					case '=':{
						SyntaxReference partition = RawSyntaxReference{
							.origin = origin,
							.boundary = {info.getDoteq() + 1, info.getLast()},
							.expression = expression,
						}.asIsolated();
						vec.append_range(partition.findMemberDeclarationsContaining(positions));
						}break;
					case '.':
						vec.push_back(info.getMember());
						break;
					default:
						throw nix::Error("Encountered a binding that is neither dot nor eq.");
				}
			}
		}
		return vec;
	}
}

// Find every place where this non-isolated value is declared! MAY miss empty declarations.
std::vector<AttributeDeclaration> SyntaxReference::findAllDeclarations() const{
	std::set<uint32_t> positions{};

	nix::ExprAttrs* parent = path->getAttrs();
	if(path->name.symbol){
		nix::ExprAttrs::AttrDef& def = parent->attrs.value()[path->name.symbol];
		positions.insert(origin.offsetOf(def.pos));
	}else{
		for(auto& dynAttr : parent->dynamicAttrs.value()){
			if(dynAttr.valueExpr == this->expression){
				positions.insert(origin.offsetOf(dynAttr.pos));
				break;
			}
		}
	}

	const nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(this->expression);

	if(attrs){
		if(attrs->pos != nix::noPos){
			positions.insert(origin.offsetOf(attrs->pos));
		}
		visitAttributeDefinitions(attrs, CollectPositions{origin, positions});
	}
	return getParent().findMemberDeclarationsContaining(positions);
}


	/*struct ListElementIter{
		const nix::PosTable::Origin& origin;
		nix::ExprList* list;
		const NixToken* begin;
		const NixToken* cursor;
		uint32_t index;
		ListElementIter& firstAdvance(){
			while(cursor->type != '['){
				cursor++; // Skip possible '(' brackets surrounding us.
			}
			advance();
			index = 0;
			return *this;
		}
		bool advance(){
			index++;
			if(index == list->elems.size()) return false;
			begin = cursor;
		expr_select:
			if(cursor->type == NixToken::kind_type::LET || cursor->type == NixToken::kind_type::REC){
				if(!maybeWalkToClosingToken(++cursor)){
					// TODO: Error.
				}
				cursor++;
			}else if(cursor->type == NixToken::kind_type::PATH || cursor->type == NixToken::kind_type::HPATH){
				walkToToken(++cursor, NixToken::kind_type::PATH_END);
				cursor++;
			}else{
				maybeWalkToClosingToken(cursor);
				cursor++;
			}
			if(cursor->type == '.'){
				while(cursor->type == '.'){
					cursor++;
					maybeWalkToClosingToken(cursor);
					cursor++;
				}
				if(cursor->type == NixToken::kind_type::OR_KW){
					cursor++;
					goto expr_select; // HAHAHAHAHAHA back to square one we go.
				}
			}else if(cursor->type == NixToken::kind_type::OR_KW){
				cursor++;
			}
			return true;
		}
		SyntaxReference current() const{
			return SyntaxReference{origin, {begin, cursor}, 0, nullptr, list->elems[index]};
		}
		std::weak_ordering operator<=>(ListElementIter that) const{
			return this->index <=> that.index;
		}
	};
	inline decltype(auto) listBegin(){
		return make_sfi(ListElementIter{
			.origin = origin,
			.list = (nix::ExprList*)this->expression,
			.begin = boundary.data(),
			.cursor = boundary.data()
		}.firstAdvance());
	}
	inline decltype(auto) listEnd(){
		return make_sfi<ListElementIter>();
	}*/

/*struct AttrDecInfo{
	// Index of parent declaration or 0xFFFFFFFF if there is none.
	uint32_t parent;
	uint32_t depth;
	// The first token of the name.
	uint32_t begin;
	// The token which is either the '.' or '=' part of the declaration, or equal to begin if this is an INHERIT declaration.
	uint32_t doteq;
	// The semicolon at the end.
	uint32_t endsemi;
};

inline std::vector<AttrDecInfo> parseAttrs(std::span<const NixToken> boundary){
	std::vector<AttrDecInfo> data;
	const uint32_t noStack = 0xFFFFFFFF;
	uint32_t stack = 0xFFFFFFFF;
	const NixToken* cursor = boundary.data() + 1; // Skip starting '{' or 'let'
#define cindex ((uint32_t)(cursor - boundary.data()))

parseAttrs:
	if(cursor->type == '}' || cursor->type == NixToken::kind_type::IN_KW){
		if(stack == noStack){
			goto endBracket;
		}
		cursor++;
		goto popStack;
	}
	if(cursor->type == NixToken::kind_type::INHERIT){
		data.push_back({.parent = stack, .depth = stack == noStack ? 0 : data[stack].depth + 1});
		stack = data.size() - 1;
		data[stack].begin = cindex;
		data[stack].doteq = cindex;
		walkToToken(++cursor, ';');
		data[stack].endsemi = cindex;
		stack = data[stack].parent;
		cursor++;
		goto parseAttrs;
	}
enterAttr:
	data.push_back({.parent = stack, .depth = stack == noStack ? 0 : data[stack].depth + 1});
	stack = data.size() - 1;
	data[stack].begin = cindex;
	maybeWalkToClosingToken(cursor);
	cursor++;
	data[stack].doteq = cindex;
	if(cursor->type == '.'){
		cursor++;
		goto enterAttr;
	}
	cursor++; // Skip '='
	if(cursor->type == NixToken::kind_type::REC){
		cursor++; // Skip 'rec' in front of an attrs
	}
	if(cursor->type == '{'){
		cursor++; // Enter a new attrs.
		goto parseAttrs;
	}
	walkToToken(cursor, ';'); // Skip a non-attrs expression.
	goto popStack;
popStack:
	// We are looking at a ';' and we must assign it's location to a number of declaration infos.
	data[stack].endsemi = cindex;
	stack = data[stack].parent;
	while(stack != noStack && boundary[data[stack].doteq].type == '.'){
		data[stack].endsemi = cindex;
		stack = data[stack].parent;
	}
	cursor++;
	goto parseAttrs;
endBracket:
	return data;
}*/

}