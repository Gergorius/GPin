#pragma once

#include <compare>
#include <nix/expr/eval-error.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/expr/value.hh>
#include <nix/util/pos-table.hh>
#include <nix/util/source-path.hh>
#include <string_view>

#include <nix/parser-tab.hh>
#include <string_view>
#include <type_traits>
using YYSTYPE = nix::parser::BisonParser::value_type;
using YYLTYPE = nix::parser::BisonParser::location_type;
#include <nix/lexer-tab.hh>

namespace nix { class Parser : public parser::BisonParser { using BisonParser::BisonParser; }; }
YY_DECL;

struct NixToken{
	using kind_type = nix::Parser::token_kind_type;
	using kind_utype = std::underlying_type_t<kind_type>;
	kind_utype type;
	uint32_t begin;
	uint32_t end;
#ifndef NDEBUG
private:
	std::span<const char> content;
#endif
public:
	NixToken() = default;
	NixToken(uint32_t b): begin(b){}
	NixToken(kind_utype t,uint32_t b,uint32_t e): type(t), begin(b), end(e){}
#ifndef NDEBUG
	NixToken(kind_utype t,uint32_t b,uint32_t e,std::string_view s): type(t), begin(b), end(e), content(s){}
#endif
	std::weak_ordering operator<=>(const NixToken& that) const{
		return this->begin <=> that.begin;
	}
};

struct ParseResult{
	nix::Expr* rootExpression;
	nix::DocCommentMap docComments;
	// Tokens sorted in encounter order.
	std::vector<NixToken> tokens;
	const nix::PosTable::Origin* origin;
	ParseResult() = default;
	explicit ParseResult(const ParseResult&) = default; // Copying must be explicit!
	ParseResult(ParseResult&&) = default;
};

ParseResult parseExprFromString(nix::EvalState& state, const nix::Pos::Origin& origin, const nix::SourcePath& basePath, nix::Exprs& exprs, std::string_view input);