#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/search-path.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/store/store-open.hh>
#include <nix/util/error.hh>
#include <nix/util/source-path.hh>
#include <ostream>
#include <sstream>

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

#include "treewalk.h"
#include "valueutil.h"

using std::span;
using std::string;
using std::string_view;

struct RewriteLink{
	std::unique_ptr<RewriteLink> next;
	std::variant<string_view,string> data = string_view();
	inline RewriteLink(): data(string_view()){}
	inline string_view view() const { return std::visit([](const auto& data){ return string_view(data); }, data); }
};

// A rewrite instruction.
struct Rewrite{
	uint32_t begin;
	uint32_t end;
	std::unique_ptr<RewriteLink> replacement;
	inline std::partial_ordering operator<=>(const Rewrite& t) const{
		const Rewrite* that = &t;
		if(this->end <= that->begin){
			return this->begin <=> that->end;
		}
		if(that->end <= this->begin){
			return this->end <=> that->begin;
		}
		return std::partial_ordering::unordered;
	}
};

inline std::ostream& operator<<(std::ostream& out,Rewrite& rw){
	RewriteLink* lnk = rw.replacement.get();
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
	using list_Rewrite = std::vector<Rewrite>;
	list_Rewrite rewrites;
	nix::EvalState& eval(){ return pool.state; }
	uint32_t resolvePos(nix::PosIdx pos) const;
};
std::pair<string_view,string_view> detectIndentation(string_view source);

void generateUpdate(RewriteState& state, SyntaxReference& expr, const nix::Bindings& bindings);