
#include "format.h"
#include "altparse.h"
#include "treewalk.h"

#include <boost/multi_index_container_fwd.hpp>
#include <cstdint>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/print.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/source-accessor.hh>
#include <nix/util/source-path.hh>
#include <nix/util/util.hh>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <boost/multi_index_container.hpp>
#include <variant>

#pragma clang diagnostic error "-Wswitch"
#pragma clang diagnostic error "-Wimplicit-fallthrough"

// TODO
// Handle unreferencable symbols in cycles
// Handle lists and primitive operator applications in cycles

namespace gpin{

// Copied over from print.cc.
bool isVarName(std::string_view s){
	if (s.size() == 0)
		return false;
	if (nix::isReservedKeyword(s))
		return false;
	char c = s[0];
	if ((c >= '0' && c <= '9') || c == '-' || c == '\'')
		return false;
	for (auto & i : s)
		if (!((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9') || i == '_' || i == '-'
				|| i == '\''))
			return false;
	return true;
}

using std::string;
using std::string_view;

IndentInfo detectIndentation(string_view source,uint32_t position){
	string_view preindent = {};
	string_view lineAtPosition = {};
	std::unordered_map<string_view, uint32_t> frequencyMap;
	std::optional<string_view> prevLine;
	using pos = string_view::size_type;

	while(source.size() != 0){
		pos c = source.find_first_of('\n');
		string_view line1;
		if(c == string_view::npos){
			line1 = source;
			source = {};
		}else{
			line1 = source.substr(0, c);
			source = source.substr(c + 1);
		}
		if(position <= line1.size()){
			lineAtPosition = line1;
			position = 0xFFFFFFFF;
		}else if(position != 0xFFFFFFFF){
			position -= (line1.size() + 1);
		}
		pos x = line1.find_first_not_of("\t ");
		if(x != string_view::npos){
			line1 = line1.substr(0,x);
		}

		if(prevLine){
			string_view line0 = prevLine.value();

			uint32_t i = 0;
			while(i < line0.size() && i < line1.size() && line0[i] == line1[i]){
				i++;
			}

			frequencyMap.try_emplace(line0.substr(i),0).first->second++;
			frequencyMap.try_emplace(line1.substr(i),0).first->second++;
			
			uint32_t j = 0;
			while(j < line1.size() && j < preindent.size() && line1[j] == preindent[j]){
				j++;
			}
			preindent = preindent.substr(0, j);
		}else{
			preindent = line1;
		}

		prevLine.emplace(line1);
	}
	
	std::optional<std::pair<string_view, uint32_t>> bestPair;

	for(auto& pair : frequencyMap){
		if(!pair.first.empty() && (!bestPair || bestPair->second < pair.second)){
			bestPair = pair;
		}
	}
	if(!bestPair){
		return {};
	}

	uint32_t repeatCount = 0;

	if(lineAtPosition.starts_with(preindent)){
		lineAtPosition = lineAtPosition.substr(preindent.size());
		while(lineAtPosition.starts_with(bestPair->first)){
			repeatCount++;
			lineAtPosition = lineAtPosition.substr(bestPair->first.size());
		}
	}
	return {preindent, bestPair->first, repeatCount};
}

IndentInfo detectIndentation(FormatState& state, uint32_t begin, uint32_t end, uint32_t focus){
	using pos = string_view::size_type;
	pos lineStart = state.source.substr(0, begin).find_last_of('\n');
	lineStart = lineStart == string_view::npos ? 0 : lineStart + 1;
	return detectIndentation(state.source.substr(lineStart, end - lineStart), focus - lineStart);
}

Cursor FormatState::addRewrite(uint32_t begin,uint32_t end){
#ifndef NDEBUG
	return Cursor(replacement.insert({RewriteRange{begin, end, offsetToNixPos(begin)}, nullptr})->second);
#else
	return Cursor(replacement.insert({RewriteRange{begin, end}, nullptr})->second);
#endif
}

void FormatState::eraseRange(uint32_t begin,uint32_t end){
#ifndef NDEBUG
	replacement.insert({RewriteRange{begin, end, offsetToNixPos(begin)}, nullptr});
#else
	replacement.insert({RewriteRange{begin, end}, nullptr});
#endif
}

static void addBrackets(FormatState& rs, Cursor& state){
	state << "(";
	Cursor temp = state.split();
	state << ")";
	state = std::move(temp);
}

static void eraseDeclaration(FormatState& state, const AttrsDeclarationInfo& info){
	state.eraseRange((info.getFirst() - 1)->end, info.getLast()->end);
}

void generateNegativeRewrites(FormatState &state, AttrsOpenInfo info, uint32_t depth, const AnchorSet &anchors){

#define RANGE_HIT(itr,end_i) ( itr != anchors.end() && itr->first < end_i )

	AttrsSyntaxBase prev = info;
	while(true){
	continueOuterLoop:
		std::optional<AttributeDeclaration> dec = std::visit(nix::overloaded{
			[](AttrsCloseInfo) { return (std::optional<AttributeDeclaration>)std::nullopt; },
			[](AttributeDeclaration ad) { return std::optional<AttributeDeclaration>(ad); }
		}, prev.getNext());
		if(!dec.has_value()){
			return;
		}
		AttributeDeclaration ad = dec.value();

		AttrsDeclarationInfo& baseInfo = std::visit([](auto& a) -> AttrsDeclarationInfo& { return a; }, ad);
		prev = baseInfo;

		auto itr = anchors.upper_bound(baseInfo.getFirst()->begin);

		// We keep inherit-froms because they cannot always be anchored properly.

		if(!std::holds_alternative<InheritFromInfo>(ad) && !RANGE_HIT(itr,baseInfo.getLast()->end)){
			eraseDeclaration(state, baseInfo);
			goto continueOuterLoop;
		}

		// We are keeping this. Look for subdeclarations.

		const NixToken* cursor = std::visit(nix::overloaded{
			[](BindingInfo& binding){ return (const NixToken*)nullptr; },
			[](InheritInfo& info){ return info.getAttrsBegin(); },
			[](InheritFromInfo& info){ return info.getAttrsBegin(); }
		}, ad);

		if(cursor == nullptr){
			BindingInfo& binding = std::get<BindingInfo>(ad);
			uint32_t innerDepth = depth + 1;

			while(binding.getDoteq()->type == '.'){
				binding = binding.getMember();
				innerDepth++;
			}

			do{
				if(innerDepth < itr->second){
					generateNegativeRewrites(state, binding.getAttrsValue(), innerDepth, anchors);
					goto continueOuterLoop;
				}
				itr++;
			}while(RANGE_HIT(itr,binding.getLast()->end));
		}else while(cursor != baseInfo.getLast()){
			itr = anchors.lower_bound(cursor->begin);
			if(!RANGE_HIT(itr, cursor->end)){
				state.eraseRange((cursor - 1)->end, cursor->end);
			}
			cursor++;
		}
	}
};

#define STREAM(os,code) [&]() -> std::string{ std::ostringstream os; code; return std::move(os).str(); }()

std::variant<IsolatedExpressionTarget,NewExpressionTarget> ExistingExpressionTarget::toIsolated(FormatState& state) &&{
	if(anchorTarget.tryIsolateAndAnchor(*this)){
		return IsolatedExpressionTarget(std::move(*this), precedence::topLevel);
	}else if(this->isInherit()){
		BaseBindingsTarget* parent = static_cast<BaseBindingsTarget*>(const_cast<SyntaxReference*>(&getParent()));
		return parent->makeNewAttribute(state, anchorTarget, this->getName().symbol);
	}else{
		std::vector<AttributeDeclaration> decvec = findAllDeclarations();
		const BindingInfo& info = std::get<BindingInfo>(decvec[0]);

		anchorTarget.addAnchorForBinding(info, *this);

		Cursor fs;
		if(info.getDoteq()->type != '='){
			fs = state.addRewrite((info.getDoteq() - 1)->end, info.getLast()->begin);
			fs << " = ";
		}else{
			fs = state.addRewrite((info.getDoteq() + 1)->begin, info.getLast()->begin);
		}
		fs.indentInfo = detectIndentation(state, boundary.begin()->begin, boundary.end()->end, info.getFirst()->begin);

		return NewExpressionTarget(std::move(fs), precedence::topLevel);
	}
};

NewExpressionTarget ExistingExpressionTarget::toSimple(FormatState& state,enum precedence maximumPrecedence) &&{
	return std::visit([&](auto x){ return std::move(x).toSimple(state, maximumPrecedence); },std::move(*this).toIsolated(state));
}

std::variant<NewListTarget> ExistingExpressionTarget::toList(FormatState& state) &&{
	return std::visit([&](auto v){
		return std::visit([&](auto k) -> std::variant<NewListTarget> {
			return std::move(k);
		}, std::move(v).toList(state));
	}, std::move(*this).toIsolated(state));
}

std::variant<IsolatedLetTarget,NewLetTarget> ExistingExpressionTarget::toLet(FormatState& state) &&{
	return std::visit([&](auto v){
		return std::visit([&](auto k) -> std::variant<IsolatedLetTarget,NewLetTarget> {
			return std::move(k);
		}, std::move(v).toLet(state));
	}, std::move(*this).toIsolated(state));
}

std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> ExistingExpressionTarget::toApp(FormatState& state) &&{
	return std::visit([&](auto v){
		return std::visit([&](auto k) -> std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> {
			return std::move(k);
		}, std::move(v).toApp(state));
	}, std::move(*this).toIsolated(state));
}

std::variant<ExistingBindingsTarget,NewBindingsTarget> ExistingExpressionTarget::toBindings(FormatState& state,bool recursive) &&{
	nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(this->expression);
	if(attrs){
		bool anchorAdded = anchorTarget.tryAddAnchorForAttrs(*this);

		if(attrs->recursive != recursive){
			const NixToken* cursor;
			std::vector<AttributeDeclaration> decvec = findAllDeclarations();
			for(const AttributeDeclaration& dec : decvec){
				const BindingInfo& info = std::get<BindingInfo>(dec);
				if(info.getDoteq()->type == '='){
					cursor = AttrsOpenInfo::nextOpen(info.getDoteq() + 1).getOpenBracket();
					if(recursive){
						if((cursor - 1)->type != NixToken::kind_type::REC){
							Cursor rec = state.addRewrite(cursor->begin);
							rec << " rec ";
						}
					}else{
						if((cursor - 1)->type == NixToken::kind_type::REC){
							state.eraseRange((cursor - 1)->begin, cursor->begin);
						}
					}
				}
			}
			BindingInfo& info = std::get<BindingInfo>(decvec[0]);
			if(recursive && (!anchorAdded || info.getDoteq()->type != '=')){
				const NixToken* insertAfter = info.getFirst() - 1;
				anchorTarget.addAnchorForInsertLocation(insertAfter->end);

				Cursor newDec = state.addRewrite(insertAfter->end);
				newDec.indentInfo = detectIndentation(state, boundary.front().begin, boundary.back().end, info.getFirst()->begin);

				newDec.newLine();
				newDec << state.sourceSpan(info.getFirst()->begin, (info.getDoteq() - 1)->end);
				newDec << " = rec {";
				Cursor body = newDec.split();
				newDec << "};";
				return ExistingBindingsTarget(std::move(*this), anchorTarget, std::move(body));
			}
		}
		return ExistingBindingsTarget(std::move(*this),anchorTarget);
	}else{
		return std::visit([](auto x) -> NewBindingsTarget { return std::move(x); }, std::move(*this).toSimple(state).toBindings(state, recursive));
	}
}

std::variant<std::monostate,IsolatedExpressionTarget,ExistingBindingsTarget> ExistingExpressionTarget::toIsolatedOrBindingsOrFinish() &&{
	if(dynamic_cast<nix::ExprAttrs*>(expression)){
		anchorTarget.tryAddAnchorForAttrs(*this);
		return ExistingBindingsTarget(std::move(*this), anchorTarget);
	}else if(anchorTarget.tryIsolateAndAnchor(*this)){
		return IsolatedExpressionTarget(std::move(*this), precedence::topLevel);
	}else{
		// We can only possibly be an inherit.
		anchorTarget.addAnchorForInheritSelector(*this);
		return std::monostate();
	}
}

NewExpressionTarget IsolatedExpressionTarget::toSimple(FormatState& state,enum precedence maxPrec) &&{
	if(maxPrec < precedence){
		const NixToken* cursor = boundary.data();
		if(cursor->type == '('){
			const NixToken* begin = cursor++;
			walkToToken(cursor, ')');
			const NixToken* end = cursor;
			if(end == boundary.data() + boundary.size() - 1){
				return NewExpressionTarget(state.addRewrite((begin + 1)->begin,(end - 1)->end), precedence::topLevel);
			}
		}
	}
	return NewExpressionTarget(state.addRewrite(beginOffset(), endOffset()), precedence).toSimple(state,maxPrec);
}

std::variant<IsolatedBindingsTarget,NewBindingsTarget> IsolatedExpressionTarget::toBindings(FormatState& state,bool recursive) &&{
	nix::ExprAttrs* attrs = dynamic_cast<nix::ExprAttrs*>(this->expression);
	if(attrs){
		if(attrs->recursive != recursive){
			const NixToken* cursor = boundary.data();
			if(attrs->recursive){
				while(cursor->type != NixToken::kind_type::REC){
					cursor++;
				}
				state.eraseRange(cursor->begin, (cursor + 1)->begin);
			}else{
				while(cursor->type != '{'){
					cursor++;
				}
				Cursor rs = state.addRewrite(cursor->begin);
				if(state.sourceSpan((cursor - 1)->end, cursor->begin).size() == 0){
					rs << " rec ";
				}else{
					rs << "rec ";
				}
			}
		}
		return IsolatedBindingsTarget(std::move(*this).toSyntaxReference());
	}else{
		return std::visit([](auto v){ return std::move(v); },std::move(*this).toSimple(state).toBindings(state, recursive));
	}
}

std::variant<IsolatedLetTarget,NewLetTarget> IsolatedExpressionTarget::toLet(FormatState& state) &&{
	if(dynamic_cast<nix::ExprLet*>(this->expression)){
		return IsolatedLetTarget(std::move(*this).toSyntaxReference());
	}else{
		return std::visit([](auto v){ return std::move(v); },std::move(*this).toSimple(state).toLet(state));
	}
}

std::variant<NewListTarget> IsolatedExpressionTarget::toList(FormatState& state) && {
	return std::move(*this).toSimple(state).toList(state);
}
std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> IsolatedExpressionTarget::toApp(FormatState& state) && {
	return std::move(*this).toSimple(state).toApp(state);
}


NewExpressionTarget NewExpressionTarget::toSimple(FormatState& state,enum precedence maxPrec) &&{
	if(maxPrec < precedence){
		cursor << "(";
		Cursor tmp = cursor.split();
		cursor << ")";
		cursor = std::move(tmp);
		precedence = precedence::topLevel;
	}
	return std::move(*this);
}

std::variant<NewBindingsTarget> NewExpressionTarget::toBindings(FormatState& state,bool recursive) &&{
	if(recursive){
		cursor << "rec {";
	}else{
		cursor << "{";
	}
	Cursor body = cursor.split();
	body.indentInfo.level++;
	cursor.newLine();
	cursor << "}";
	return NewBindingsTarget(std::move(body));
}

std::variant<NewListTarget> NewExpressionTarget::toList(FormatState& state) &&{
	return NewListTarget(std::move(cursor));
}

std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> NewExpressionTarget::toApp(FormatState& state) &&{
	if(precedence::app < precedence){
		return std::move(*this).toSimple(state, precedence::app).toApp(state);
	}
	Cursor lBody = cursor.split();
	cursor << " ";
	return std::pair{
		NewExpressionTarget(std::move(lBody),precedence::app),
		NewExpressionTarget(std::move(cursor),precedence::after_app)
	};
}

std::variant<NewLetTarget> NewExpressionTarget::toLet(FormatState& state) &&{
	cursor << "let";
	return NewLetTarget(std::move(cursor));
}

NewExpressionTarget NewLetTarget::finish(FormatState& state) &&{
	cursor.indentInfo.level--;
	cursor.newLine();
	cursor << "in";
	cursor.indentInfo.level++;
	cursor.newLine(true);
	return NewExpressionTarget(std::move(cursor), precedence::function);
}

std::variant<NewExpressionTarget> NewListTarget::nextExpr(){
	if(elemCount == 0){
		cursor << "[";
		Cursor body = cursor.split();
		body.indentInfo.level++;
		body.newLine();
		cursor.newLine();
		cursor << "]";
		cursor = std::move(body);
	}else{
		cursor.newLine(true);
	}
	elemCount++;
	return NewExpressionTarget(cursor.split(), precedence::select);
}

void NewListTarget::finish(FormatState& state) &&{
	if(elemCount == 0){
		cursor << "[]";
	}
}

NewExpressionTarget BaseBindingsTarget::makeNewAttribute(FormatState& state, AnchorSet& anchorSet, nix::Symbol name){
	bool needDelimeter = false;
	if(!insertionPoint.isValid()){
		const NixToken* cursor;
		if(isIsolated()){ // Always the case for let.
			cursor = AttrsOpenInfo::nextOpen(boundary.data()).getOpenBracket();
			goto positionInsideSet;
		}else{
			std::vector<AttributeDeclaration> decvec = findAllDeclarations();

			for(AttributeDeclaration& dec : decvec){
				// We know for a fact this can only be a binding info.
				const BindingInfo& info = std::get<BindingInfo>(dec);
				if(info.getDoteq()->type == '='){
					cursor = AttrsOpenInfo::nextOpen(info.getDoteq() + 1).getOpenBracket();
					goto positionInsideSet;
				}
			}

			Cursor cur = getParent().makeNewAttribute(state, anchorSet, this->getName().symbol).toCursor();
			cur << "{";
			insertionPoint = cur.split();
			insertionPoint.indentInfo.level++;
			cur.newLine();
			cur << "}";
			goto makeNewExpression;
		}
	positionInsideSet:
		const NixToken* begin = cursor;
		maybeWalkToClosingToken(cursor);
		const NixToken* end = cursor;
		const NixToken* insertAfter = end - 1;

		needDelimeter = insertAfter->type == NixToken::kind_type::LET;

		anchorSet.addAnchorForInsertLocation(insertAfter->end);

		Cursor body = state.addRewrite(insertAfter->end);
		body.indentInfo = detectIndentation(state, begin->begin, end->end, insertAfter->end);

		if(begin->end == end->begin){
			Cursor inner = body.split();
			body.newLine();
			inner.indentInfo.level++;
			insertionPoint = std::move(inner);
		}else{
			insertionPoint = std::move(body);
		}
	}else if(insertionPoint.lookahead("}")){
		Cursor tmp = insertionPoint.split();
		tmp.indentInfo.level++;
		insertionPoint.newLine();
		insertionPoint = std::move(tmp);
	}
makeNewExpression:
	insertionPoint.newLine(needDelimeter);
	insertionPoint << STREAM(os,nix::printAttributeName(os, state.eval().symbols[name]));
	insertionPoint << " = ";
	Cursor body = insertionPoint.split();
	insertionPoint << ";";
	return NewExpressionTarget(std::move(body), precedence::topLevel);
}

std::optional<IsolatedExpressionTarget> BaseBindingsTarget::getAndAnchorInheritFromExpression(uint32_t index, AnchorSet& anchors){
	std::optional<SyntaxReference> sr = this->getInheritFromExpression(index);
	if(sr){
		anchors.addAnchorForInheritFrom(sr.value());
		return IsolatedExpressionTarget(std::move(sr).value(), precedence::topLevel);
	}
	return std::nullopt;
}

std::pair<RawSyntaxReference,ExistingExpressionTarget> BaseBindingsTarget::getDynamicAttribute(uint32_t index, AnchorSet& anchors){
	auto& dynAttr = attrs()->dynamicAttrs->at(index);
	return {this->rawWithExpression(dynAttr.nameExpr), ExistingExpressionTarget(getDynamicSubexpression(index), anchors)};
}

IsolatedExpressionTarget IsolatedLetTarget::finishNoCleanup(FormatState& state) &&{
	return IsolatedExpressionTarget(this->getBody(), precedence::function);
}

IsolatedExpressionTarget IsolatedLetTarget::finish(FormatState& state) &&{
	generateNegativeRewrites(state, AttrsOpenInfo::nextOpen(boundary.data()), 0, anchorSet);
	return std::move(*this).finishNoCleanup(state);
}

std::variant<ExistingExpressionTarget,NewExpressionTarget> BaseBindingsTarget::makeStaticAttribute(FormatState& state, nix::Symbol name, AnchorSet& anchors){
	nix::ExprAttrs* a = this->attrs();
	auto itr = a->attrs->find(name);
	if(itr != a->attrs->end()){
		return ExistingExpressionTarget(this->getSubexpression(name), anchors);
	}else{
		return this->makeNewAttribute(state, anchors, name);
	}
}

std::variant<ExistingExpressionTarget,NewExpressionTarget> ExistingBindingsTarget::makeStaticAttribute(FormatState& state, nix::Symbol name){
	return BaseBindingsTarget::makeStaticAttribute(state, name, anchorSet);
}

std::variant<ExistingExpressionTarget,NewExpressionTarget> IsolatedBindingsTarget::makeStaticAttribute(FormatState& state, nix::Symbol name){
	return BaseBindingsTarget::makeStaticAttribute(state, name, anchorSet);
}

std::optional<IsolatedExpressionTarget> ExistingBindingsTarget::getAndAnchorInheritFromExpression(uint32_t index){
	return BaseBindingsTarget::getAndAnchorInheritFromExpression(index, anchorSet);
}

std::optional<IsolatedExpressionTarget> IsolatedBindingsTarget::getAndAnchorInheritFromExpression(uint32_t index){
	return BaseBindingsTarget::getAndAnchorInheritFromExpression(index, anchorSet);
}

std::pair<RawSyntaxReference,ExistingExpressionTarget> ExistingBindingsTarget::getDynamicAttribute(uint32_t index){
	return BaseBindingsTarget::getDynamicAttribute(index, anchorSet);
}

std::pair<RawSyntaxReference,ExistingExpressionTarget> IsolatedBindingsTarget::getDynamicAttribute(uint32_t index){
	return BaseBindingsTarget::getDynamicAttribute(index, anchorSet);
}

void IsolatedBindingsTarget::finish(FormatState& state) &&{
	generateNegativeRewrites(state, AttrsOpenInfo::nextOpen(boundary.data()), 0, anchorSet);
}

std::variant<NewExpressionTarget> NewBindingsTarget::makeStaticAttribute(FormatState& state, nix::Symbol name){
	cursor.newLine(cursor.lookbehind("let"));
	cursor << STREAM(os,nix::printAttributeName(os, state.eval().symbols[name]));
	cursor << " = ";
	NewExpressionTarget newExpr = NewExpressionTarget(cursor.split(), precedence::topLevel);
	cursor << ";";
	return newExpr;
}

}