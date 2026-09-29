
#include <compare>
#include <cstdint>
#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-error.hh>
#include <nix/expr/search-path.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/store/store-open.hh>
#include <nix/util/error.hh>
#include <nix/util/source-path.hh>
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
#include <vector>

#include "treewalk.h"

using std::string;
using std::string_view;

// A rewrite instruction.
struct Rewrite{
	uint32_t begin;
	uint32_t end;
	string replacement;
	std::partial_ordering operator<=>(const Rewrite& t) const{
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

struct RewriteState{
	nix::EvalState& eval;
	// String to use for indentation. It is a part of source.
	const string_view defaultIndent;
	const string_view source;
	const nix::SourcePath& basePath;
	std::vector<Rewrite> rewrites;
	inline void addRewrite(Rewrite rw){
		rewrites.push_back(std::move(rw));
	}
	uint32_t resolvePos(nix::PosIdx pos) const;
};

inline std::pair<string_view,string_view> detectIndentation(string_view source);

std::pair<string_view,string_view> detectIndentation(RewriteState& state, uint32_t begin, uint32_t end);

struct FormatState{
	nix::EvalState& eval;
	uint32_t begin;
	uint32_t end;
	string_view preindent;
	string_view indent;
	int32_t indentRepeatCount;
	const nix::SourcePath& basePath;
	std::ostringstream output;
	nix::ValueType parentValueType;
	bool newLine();
	Rewrite toRewrite() &&;
};

// Generate rewrites that completely replaces the specified expression with the specified replacement value.
void generateRewrite(RewriteState& state, SyntaxReference expr, nix::Value& value);