#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/search-path.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/store/store-open.hh>
#include <nix/util/error.hh>
#include <nix/util/source-path.hh>
#include <optional>
#include <ostream>

#include <nix/expr/nixexpr.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/value.hh>
#include <nix/expr/print.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/pos-table.hh>
#include <nix/util/position.hh>
#include <boost/dynamic_bitset.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

#include "altparse.h"
#include "treewalk.h"

namespace gpin{

using std::span;
using std::string;
using std::string_view;

struct IndentInfo{
	// A series of whitespace characters present at the start of every line.
	string_view preindent;
	// A series of whitespace characters representing one level of indentation.
	string_view indent;
	// Current indentation level.
	uint32_t level;
};

struct RewriteLink{
	std::unique_ptr<RewriteLink> next;
	std::variant<string_view,string> data = string_view();
	inline RewriteLink(): data(string_view()){}
	inline string_view view() const { return std::visit([](const auto& data){ return string_view(data); }, data); }
};

struct RewriteRange{
	const uint32_t begin;
	const uint32_t end;
#ifndef NDEBUG
	const nix::Pos pos;
	inline RewriteRange(uint32_t b,uint32_t e,nix::Pos p): begin(b), end(e), pos(p)
#else
	inline RewriteRange(uint32_t b,uint32_t e): begin(b), end(e)
#endif
	{
		if(e < b){
			throw std::exception();
		}
	}
};

struct CompareRanges{
	inline bool operator()(const RewriteRange& l, const RewriteRange& r) const{
		if(l.end <= r.begin){
			return l.begin < r.end;
		}
		if(r.end <= l.begin){
			return l.end < r.begin;
		}
#ifndef NDEBUG
		std::ostringstream lout,rout;
		l.pos.print(lout, false);
		r.pos.print(rout, false);
		throw nix::Error("Overlapping rewrites: [%5%](%1% %2%) [%6%](%3% %4%)",l.begin,l.end,r.begin,r.end,lout.str(),rout.str());
#else
		throw nix::Error("Overlapping rewrites: (%1% %2%) (%3% %4%)",l.begin,l.end,r.begin,r.end);
#endif
	}
};

inline std::ostream& operator<<(std::ostream& out,RewriteLink* lnk){
	while(lnk != nullptr){
		out << lnk->view();
		lnk = lnk->next.get();
	}
	return out;
}

struct Cursor{
	friend struct FormatState;
	RewriteLink* targetLink;
	IndentInfo indentInfo;
private:
	inline Cursor(std::unique_ptr<RewriteLink>& t){
		t.reset(targetLink = new RewriteLink());
	}
public:
	inline Cursor(): targetLink(nullptr){}
	inline Cursor(Cursor&& that){
		targetLink = that.targetLink;
		indentInfo = that.indentInfo;
		that.targetLink = nullptr;
	}
	inline Cursor(const Cursor&) = delete;
	inline Cursor& operator=(Cursor&& that){
		targetLink = that.targetLink;
		indentInfo = that.indentInfo;
		that.targetLink = nullptr;
		return *this;
	}
	inline Cursor& operator=(const Cursor&) = delete;
private:
	inline RewriteLink& push(){
		RewriteLink* nl = new RewriteLink();
		nl->next = std::move(targetLink->next);
		targetLink->next.reset(nl);
		targetLink = nl;
		return *nl;
	}
public:
	// Tests if the text behind the cursor matches the argument. False negatives are possible because as text is inserted, preceeding text "goes out of view" and the cursor will no longer be able to read it. Text inserted by the latest insert operation will always be in view.
	inline bool lookbehind(std::string_view content) const{
		return targetLink->view().ends_with(content);
	}
	// Tests if the text in front of the cursor matches the argument. False negatives are possible because text already existing in the source or being inserted by independent cursors is "out of view" and the cursor is unable to read it.
	inline bool lookahead(std::string_view content) const{
		const RewriteLink* st = targetLink->next.get();
		while(content.length() != 0){
			if(st == nullptr){
				return false;
			}
			std::string_view stv = st->view();
			if(content.size() <= stv.size()){
				return stv.starts_with(content);
			}else if(!stv.starts_with(content)){
				return false;
			}
			content = content.substr(stv.size());
			st = st->next.get();
		}
		return true;
	}
	inline bool isValid(){
		return targetLink != nullptr;
	}
	// Split the insertion cursor into two. Everything added to the new returned cursor will precede anything subsequently added to this cursor. The lifetimes of the cursors are not interrelated.
	inline Cursor split(){
		Cursor temp;
		temp.targetLink = targetLink;
		temp.indentInfo = indentInfo;
		push();
		return temp;
	}
	inline Cursor& operator<<(string str){
		if(str.empty()){ return *this; }
		if(targetLink->view().empty()){
			targetLink->data = std::move(str);
			return *this;
		}
		if(std::holds_alternative<string>(targetLink->data)){
			std::string& s = std::get<string>(targetLink->data);
			if(s.size() + str.size() <= s.capacity()){
				s += std::move(str);
				return *this;
			}
		}
		push().data = std::move(str);
		return *this;
	}
	inline Cursor& operator<<(string_view view){
		if(view.empty()){ return *this; }
		if(targetLink->view().empty()){
			targetLink->data = view;
			return *this;
		}
		if(view.size() < 0x10 && std::holds_alternative<string>(targetLink->data)){
			std::string& s = std::get<string>(targetLink->data);
			if(s.size() + view.size() <= s.capacity()){
				s += view;
				return *this;
			}
		}
		push().data = view;
		return *this;
	}
	inline void newLine(bool alwaysDelimit = false){
		if(indentInfo.indent.size() == 0){
			if(alwaysDelimit){
				*this << " ";
			}
		}else{
			*this << "\n" << indentInfo.preindent;
			for(int32_t i = 0;i < indentInfo.level;i++){
				*this << indentInfo.indent;
			}
		}
	}
	template<size_t N>
	inline Cursor& operator<<(const char (&str)[N]){
		return this->operator<<(string_view(str));
	}
};

struct FormatState{
	nix::EvalState& evalState;
	const nix::PosTable::Origin& origin;
	const string_view source;
	const nix::SourcePath& basePath;

	// We permit multiple rewrites at the same location but we do not permit overlaps.
	std::multimap<RewriteRange,std::unique_ptr<RewriteLink>,CompareRanges> replacement;

	inline nix::EvalState& eval() const { return evalState; }
	inline string_view sourceSpan(uint32_t begin,uint32_t end) const { return source.substr(begin, end - begin); }
	inline string_view tokenContent(const NixToken& token) const { return source.substr(token.begin,token.end - token.begin); }
	inline nix::Pos offsetToNixPos(uint32_t offset) const { return eval().positions[eval().positions.add(origin, offset)]; }
	Cursor addRewrite(uint32_t begin,uint32_t end);
	inline Cursor addRewrite(uint32_t begin){ return addRewrite(begin, begin); }
	void eraseRange(uint32_t begin,uint32_t end);
};

std::pair<string_view,string_view> detectIndentation(string_view source);

struct AnchorSet : std::map<uint32_t,uint32_t>{
private:
	static constexpr uint32_t maxDepth = 0xFFFFFFFFu;
	inline void addAnchor(uint32_t pos,uint32_t depth){
		auto itr = this->insert({pos,depth}).first;
		itr->second = std::max(itr->second, depth);
	}
public:
	// Attempts to isolate the syntax and if successful, adds an anchor.
	inline bool tryIsolateAndAnchor(SyntaxReference& src){
		uint32_t depthBeforeIsolation = src.getPathLength();
		bool success = src.tryIsolate();
		if(success){
			addAnchor(src.beginOffset(), depthBeforeIsolation);
		}
		return success;
	}
	// Add an anchor for a syntax representing an inherit selector.
	inline void addAnchorForInheritSelector(const SyntaxReference& src){
		addAnchor(src.resolvePos(src.expression->getPos()), maxDepth);
	}
	// Add an anchor for a syntax representing the inner expression of inherit-from.
	inline void addAnchorForInheritFrom(const SyntaxReference& src){
		// ...which we know is always root and also not traversed by the negative rewrite algorithm.
		addAnchor(src.beginOffset(), maxDepth);
	}
	// Anchor a valid location of insertion for an attribute set. This method is for the formatter only.
	inline void addAnchorForInsertLocation(uint32_t pos){
		addAnchor(pos, maxDepth);
	}
	// Anchor a binding. The binding will not be altered by the negative rewriter.
	inline void addAnchorForBinding(const BindingInfo& binding, const SyntaxReference& src){
		addAnchor((binding.getDoteq() - 1)->end, src.getPathLength());
	}
	// Try to add an anchor for the attribute set. This always succeeds for isolatable attribute sets.
	inline bool tryAddAnchorForAttrs(const SyntaxReference& src){
		nix::PosIdx pos = src.expression->getPos();
		if(pos == nix::noPos){
			return false;
		}
		addAnchor(src.resolvePos(pos), maxDepth);
		return true;
	}
};

enum class precedence : char{
	topLevel = 0,
	function = topLevel,
	ifthen,
	pipe_from,
	pipe_to,
	op,
	app,
	after_app,
	select = after_app,
	simple,
	bottomLevel = simple
};

struct ExistingBindingsTarget;
struct ExistingExpressionTarget;
struct IsolatedLetTarget;
struct IsolatedBindingsTarget;
struct IsolatedExpressionTarget;
struct NewExpressionTarget;
struct NewBindingsTarget;
struct NewLetTarget;
struct NewListTarget;

using indentContext = std::optional<std::pair<uint32_t,uint32_t>>;

struct BaseBindingsTarget : SyntaxReference{
	Cursor insertionPoint;
	NewExpressionTarget makeNewAttribute(FormatState& state, AnchorSet& anchorSet, nix::Symbol name);
private:
	inline BaseBindingsTarget& getParent(){
		return *static_cast<BaseBindingsTarget*>(const_cast<SyntaxReference*>(&SyntaxReference::getParent()));
	}
public:
	explicit BaseBindingsTarget(SyntaxReference&& sr): SyntaxReference(std::move(sr)){}
	explicit BaseBindingsTarget(SyntaxReference&& sr, Cursor ip): SyntaxReference(std::move(sr)), insertionPoint(std::move(ip)){}
	std::variant<ExistingExpressionTarget,NewExpressionTarget> makeStaticAttribute(FormatState& state, nix::Symbol name, AnchorSet& anchors);
	std::optional<IsolatedExpressionTarget> getAndAnchorInheritFromExpression(uint32_t index, AnchorSet& anchors);
	std::pair<RawSyntaxReference,ExistingExpressionTarget> getDynamicAttribute(uint32_t index, AnchorSet& anchors);
};

// This is a bindings which is not isolated
class ExistingBindingsTarget : private BaseBindingsTarget{
	friend class IsolatedLetTarget;
	friend class ExistingExpressionTarget;
	AnchorSet& anchorSet;
	inline explicit ExistingBindingsTarget(SyntaxReference&& sr, AnchorSet& as): BaseBindingsTarget(std::move(sr)), anchorSet(as){}
	inline explicit ExistingBindingsTarget(SyntaxReference&& sr, AnchorSet& as, Cursor ip): BaseBindingsTarget(std::move(sr), std::move(ip)), anchorSet(as){}
public:
	using RawSyntaxReference::attrs;
	inline bool isRecursive(){ return attrs()->recursive; }
	std::optional<IsolatedExpressionTarget> getAndAnchorInheritFromExpression(uint32_t index);
	// This is only safe to call ONCE per symbol.
	std::variant<ExistingExpressionTarget,NewExpressionTarget> makeStaticAttribute(FormatState& state, nix::Symbol name);
	// Unfortunately I can only serve you raw syntax for the name in these trying times.
	std::pair<RawSyntaxReference,ExistingExpressionTarget> getDynamicAttribute(uint32_t index);
	inline void finish(FormatState& state) &&{}
};

// This is a bindings which IS isolated
class IsolatedBindingsTarget : private BaseBindingsTarget{
	friend class IsolatedLetTarget;
	AnchorSet anchorSet;
public:
	using RawSyntaxReference::attrs;
	inline bool isRecursive(){ return attrs()->recursive; }
	inline explicit IsolatedBindingsTarget(SyntaxReference&& sr): BaseBindingsTarget(std::move(sr)){};
	std::optional<IsolatedExpressionTarget> getAndAnchorInheritFromExpression(uint32_t index);
	// This is only safe to call ONCE per symbol.
	std::variant<ExistingExpressionTarget,NewExpressionTarget> makeStaticAttribute(FormatState& state, nix::Symbol name);
	// Unfortunately I can only serve you raw syntax for the name in these trying times.
	std::pair<RawSyntaxReference,ExistingExpressionTarget> getDynamicAttribute(uint32_t index);
	void finish(FormatState& state) &&;
};

// This is a let which IS ALWAYS isolated
class IsolatedLetTarget : private IsolatedBindingsTarget{
public:
	using IsolatedBindingsTarget::makeStaticAttribute;
	using IsolatedBindingsTarget::getAndAnchorInheritFromExpression;
	using RawSyntaxReference::attrs;
	using IsolatedBindingsTarget::isRecursive;
	inline explicit IsolatedLetTarget(SyntaxReference&& sr): IsolatedBindingsTarget(std::move(sr)){}
	IsolatedExpressionTarget finish(FormatState& state) &&;
	IsolatedExpressionTarget finishNoCleanup(FormatState& state) &&;
};

// This is a possibly non-isolated expression. Decision to isolate or explore bindings pending. Precedence is always top level.
class ExistingExpressionTarget : private SyntaxReference{
	AnchorSet& anchorTarget;
public:
	static constexpr bool exists(){ return true; }
	nix::Expr* getExpression(){ return ((SyntaxReference&)*this).expression; }
	inline explicit ExistingExpressionTarget(SyntaxReference&& sr, AnchorSet& as): SyntaxReference(std::move(sr)), anchorTarget(as){}
	std::variant<IsolatedExpressionTarget,NewExpressionTarget> toIsolated(FormatState& state) &&;
	std::variant<std::monostate,IsolatedExpressionTarget,ExistingBindingsTarget> toIsolatedOrBindingsOrFinish() &&;
	NewExpressionTarget toSimple(FormatState& state,enum precedence maximumPrecedence = precedence::bottomLevel) &&;
	std::variant<ExistingBindingsTarget,NewBindingsTarget> toBindings(FormatState& state,bool recursive) &&;
	std::variant<IsolatedLetTarget,NewLetTarget> toLet(FormatState& state) &&;
	std::variant<NewListTarget> toList(FormatState& state) &&;
	std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> toApp(FormatState& state) &&;
};

// Isolated expression with a dynamic precedence.
class IsolatedExpressionTarget : private RawSyntaxReference{
	precedence precedence;
public:
	static constexpr bool exists(){ return true; }
	const RawSyntaxReference& getSyntaxReference(){ return *this; }
	SyntaxReference toSyntaxReference() && { return std::move(*this).asIsolated(); }
	nix::Expr* getExpression(){ return this->expression; }
	inline explicit IsolatedExpressionTarget(SyntaxReference&& sr, enum precedence prec): RawSyntaxReference(std::move(sr)), precedence(prec){}
	NewExpressionTarget toSimple(FormatState& state,enum precedence maximumPrecedence = precedence::bottomLevel) &&;
	inline std::variant<IsolatedExpressionTarget> toIsolated(FormatState& state) && { return std::move(*this); }
	std::variant<IsolatedBindingsTarget,NewBindingsTarget> toBindings(FormatState& state,bool recursive) &&;
	std::variant<IsolatedLetTarget,NewLetTarget> toLet(FormatState& state) &&;
	std::variant<NewListTarget> toList(FormatState& state) &&;
	std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> toApp(FormatState& state) &&;
};

#define DECLARE_SET_INDENT_INFO(name) \
inline name& setIndentInfo(const IndentInfo& ii) &{ \
	cursor.indentInfo = ii; \
	return *this; \
} \
inline name setIndentInfo(const IndentInfo& ii) &&{ \
	cursor.indentInfo = ii; \
	return std::move(*this); \
}

class NewBindingsTarget{
	friend class NewLetTarget;
	Cursor cursor;
public:
	DECLARE_SET_INDENT_INFO(NewBindingsTarget);
	explicit NewBindingsTarget(Cursor cur): cursor(std::move(cur)){}
	// This is only safe to call ONCE per symbol.
	std::variant<NewExpressionTarget> makeStaticAttribute(FormatState& state, nix::Symbol name);
	inline void finish(FormatState& state) &&{}
};

class NewLetTarget : public NewBindingsTarget{
public:
	DECLARE_SET_INDENT_INFO(NewLetTarget);
	using NewBindingsTarget::makeStaticAttribute;
	inline explicit NewLetTarget(Cursor bic): NewBindingsTarget(std::move(bic)){}
	NewExpressionTarget finish(FormatState& state) &&;
};

class NewExpressionTarget{
	Cursor cursor;
	precedence precedence;
public:
	DECLARE_SET_INDENT_INFO(NewExpressionTarget);
	static constexpr bool exists(){ return false; }
	static constexpr nix::Expr* getExpression(){ return nullptr; }
	inline explicit NewExpressionTarget(Cursor cur,enum precedence prec): cursor(std::move(cur)), precedence(prec){}
	inline std::variant<NewExpressionTarget> toIsolated(FormatState& state) && { return std::move(*this); }
	NewExpressionTarget toSimple(FormatState& state,enum precedence maximumPrecedence = precedence::bottomLevel) &&;
	Cursor toCursor() && { return std::move(cursor); }
	std::variant<NewBindingsTarget> toBindings(FormatState& state,bool recursive) &&;
	std::variant<NewLetTarget> toLet(FormatState& state) &&;
	std::variant<NewListTarget> toList(FormatState& state) &&;
	std::variant<std::pair<NewExpressionTarget,NewExpressionTarget>> toApp(FormatState& state) &&;
};

class NewListTarget{
	Cursor cursor;
	uint32_t elemCount;
public:
	DECLARE_SET_INDENT_INFO(NewListTarget);
	inline explicit NewListTarget(Cursor cur): cursor(std::move(cur)), elemCount(0){}
	std::variant<NewExpressionTarget> nextExpr();
	void finish(FormatState& state) &&;
};

bool isVarName(std::string_view s);

IndentInfo detectIndentation(string_view source,uint32_t position);

IndentInfo detectIndentation(FormatState& state, uint32_t begin, uint32_t end, uint32_t focus);

}