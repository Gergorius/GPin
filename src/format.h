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
#include <string>
#include <string_view>
#include <variant>

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

struct UIntRange{
	const uint32_t begin;
	const uint32_t end;
	UIntRange(uint32_t b,uint32_t e): begin(b), end(e){
		if(e < b){
			throw std::exception();
		}
	}
};

struct CompareRanges{
	bool operator()(const UIntRange& l, const UIntRange& r) const{
		if(l.end <= r.begin){
			return l.begin < r.end;
		}
		if(r.end <= l.begin){
			return l.end < r.begin;
		}
		throw nix::Error("Overlapping rewrites: (%1% %2%) (%3% %4%)",l.begin,l.end,r.begin,r.end);
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
	// String to use for indentation. It is a part of source.
	const string_view defaultIndent;
	const string_view source;
	const nix::SourcePath& basePath;

	// We permit multiple rewrites at the same location but we do not permit overlaps.
	std::multimap<UIntRange,std::unique_ptr<RewriteLink>,CompareRanges> replacement;

	nix::EvalState& eval(){ return pool.state; }
	uint32_t resolvePos(nix::PosIdx pos) const;
};
std::pair<string_view,string_view> detectIndentation(string_view source);

struct AnchorSet : std::map<uint32_t,uint32_t>{
	// Indicates that declared at the specified location and depth is a value that must be kept.
	void addAnchor(uint32_t pos,uint32_t depth){
		auto itr = this->insert({pos,depth}).first;
		itr->second = std::max(itr->second, depth);
	}
	// Indicates that declared at the specified location is an attribute set that must be kept.
	void addAnchorForAttrs(uint32_t pos){
		(*this)[pos] = 0xFFFFFFFFu;
	}
};

// So how do these methods work?

// The syntax reference must be to an isolated ExprAttrs or ExprLet. This erases EVERY declaration and subdeclaration that isn't anchored.
void generateNegativeRewrites(RewriteState& state, SyntaxReference expr, const AnchorSet& anchors);
// Rewrites the value of the SyntaxReference to the specified value. This will not erase unused declarations, but instead registers anchors to used declarations to the anchor set.
void generatePositiveRewrites(RewriteState& state, SyntaxReference expr, const nix::Value& value, AnchorSet& anchors);
// Rewrites each attribute of the ExprAttrs according to the bindings.
void generatePositiveRewrites(RewriteState& state, SyntaxReference& expr, const nix::Bindings& bindings, AnchorSet& anchors);
// 
void generatePreservingAnchors(RewriteState& state, SyntaxReference expr, AnchorSet& anchors);

}