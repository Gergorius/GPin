
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <functional>
#include <map>
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
#include <span>
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
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "altparse.h"
#include "treewalk.h"

using std::string;
using std::string_view;

// A rewrite instruction.
struct Rewrite{
	uint32_t begin;
	uint32_t end;
	string replacement;
};

struct RewriteState{
	nix::EvalState& eval;
	// String to use for indentation. It is a part of source.
	const string_view defaultIndent;
	const string_view source;
	const nix::SourcePath& sourcePath;
	const ParseResult& parse;
	std::vector<Rewrite> rewrites;
	inline void addRewrite(Rewrite rw){
		rewrites.push_back(std::move(rw));
	}
	uint32_t resolvePos(nix::PosIdx pos) const;
};

inline std::pair<string_view,string_view> detectIndentation(string_view source);

std::pair<string_view,string_view> detectIndentation(RewriteState& state, uint32_t begin, uint32_t end);

struct chart_info{
	std::vector<nix::Symbol> path;
	uint32_t strength; // Already existing paths take priority over paths we make.
	inline chart_info(): path(), strength(0){}
};

inline void chart(nix::Expr* expr, nix::Value* value, std::vector<nix::Symbol>& path, std::unordered_map<nix::Value*, chart_info>& paths);

struct FormatState{
	nix::EvalState& eval;
	uint32_t begin;
	uint32_t end;
	string_view baseIndent;
	string_view repeatingIndent;
	int32_t indentRepeatCount;
	std::ostringstream output;
	nix::ValueType parentValueType;
	bool newLine();
	Rewrite toRewrite() &&;
};

inline void formatBinding(FormatState& state, const nix::Attr& attr);

inline void formatValue(FormatState& state, nix::Value& value);

// Erase a declaration.
inline void eraseDeclaration(RewriteState& state, const AttrsDeclarationInfo& info);

// Erase a non-isolated expression. Isolated expressions CANNOT be erased.
inline void eraseNonIsolated(RewriteState& state, const SyntaxReference& expr);

// Isolate the expression using rewrites. This returns a range in the input string that should be fully rewritten by the caller. Should be used only when completely rewriting the expression.
inline std::pair<uint32_t,uint32_t> isolateWithRewrite(RewriteState& state, SyntaxReference& expr);

// Erase all dynamic and inherit attributes from the attrs expression.
inline void eraseDynamicAndInheritAttrs(RewriteState& state, SyntaxReference& expr);

// Locate or make space where new bindings can be inserted into the attribute set. After this, the related finishing method must be called that inserts the necessary closing braces.
inline uint32_t findOrDeclareAttributeSet(RewriteState& rs, FormatState& state, const SyntaxReference& expr);

// Place empty braces.
inline void finishAttributeSetDeclaration(FormatState& state, uint32_t depth);

// Generate rewrites that completely replaces the specified expression with the specified replacement value.
void generateRewrite(RewriteState& state, SyntaxReference expr, nix::Value& value);