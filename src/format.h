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
#include "valueutil.h"

namespace gpin{

using std::span;
using std::string;
using std::string_view;

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

struct RewriteState{
	NixValueInternPool& pool;
	NixValueCycles& cycles;
	nix::EvalState& evalState;
	const nix::PosTable::Origin& origin;
	// String to use for indentation. It is a part of source.
	const string_view defaultIndent;
	const string_view source;
	const nix::SourcePath& basePath;

	// We permit multiple rewrites at the same location but we do not permit overlaps.
	std::multimap<RewriteRange,std::unique_ptr<RewriteLink>,CompareRanges> replacement;

	inline nix::EvalState& eval() const { return evalState; }
	inline string_view sourceSpan(uint32_t begin,uint32_t end) const { return source.substr(begin, end - begin); }
	inline string_view tokenContent(const NixToken& token) const { return source.substr(token.begin,token.end - token.begin); }
	inline nix::Pos offsetToNixPos(uint32_t offset) const { return eval().positions[eval().positions.add(origin, offset)]; }
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
		// ...which we know is always isolated and also not traversed by the negative rewrite algorithm.
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

// So how do these methods work?

// The AttrsOpenInfo must be to an isolated ExprAttrs or ExprLet. This erases EVERY declaration and subdeclaration that isn't anchored.
void generateNegativeRewrites(RewriteState &state, AttrsOpenInfo info, uint32_t depth, const AnchorSet &anchors);
// Rewrites the value of the SyntaxReference to the specified value. This will not erase unused declarations, but instead registers anchors to used declarations to the anchor set.
void generatePositiveRewrites(RewriteState& state, SyntaxReference expr, const nix::Value& value, AnchorSet& anchors);
// Rewrites each attribute of the ExprAttrs according to the bindings.
void generatePositiveRewrites(RewriteState& state, SyntaxReference& expr, const nix::Bindings& bindings, AnchorSet& anchors);

//void generatePreservingAnchors(RewriteState& state, SyntaxReference expr, AnchorSet& anchors);

}