
#include "altparse.h"

#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/parser-state.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/lexer-tab.hh>
#include <nix/parser-tab.hh>
#include <nix/util/pos-idx.hh>
#include <nix/util/pos-table.hh>
#include <optional>
#include <string_view>

#include "steal.h"

namespace gpin{

EXPORT_PRIVATE_MEMBER(positionToDocComment, &nix::EvalState::positionToDocComment);
EXPORT_PRIVATE_MEMBER(posTableResolve, &nix::PosTable::resolve);

static void prepareParseData(std::string_view input,std::vector<char>& pdata){
	pdata.clear();
	pdata.reserve(input.length() + 2);
	pdata.append_range(input);
	pdata.push_back('\0');
	pdata.push_back('\0');
}

struct yyscanner{
	std::optional<yyscan_t> payload;
	yyscanner(){}
	yyscanner(nix::LexerState* def){
		payload.emplace();
		yylex_init_extra(def, &payload.value());
	}
	yyscanner(const yyscanner&) = delete;
	yyscanner& operator=(const yyscanner&) = delete;
	yyscanner& operator=(yyscanner&& that){
		reset();
		this->payload = that.payload;
		that.payload.reset();
		return *this;
	}
	void reset(){
		if(payload){
			yylex_destroy(payload.value());
			payload.reset();
		}
	}
	~yyscanner(){ reset(); }
	operator yyscan_t(){ return payload.value(); }
};

void parseExprFromString(ParseResult& result, nix::EvalState& state, const nix::Pos::Origin& origin, const nix::SourcePath& basePath, nix::Exprs& exprs, std::string_view input){

	{
		nix::LexerState lexerState{
			.positionToDocComment = result.docComments,
			.positions = state.positions,
			.origin = state.positions.addOrigin(origin, input.length() + 2),
		};
		result.origin = std::invoke(posTableResolve,state.positions,state.positions.add(lexerState.origin, 0));
		nix::ParserState parserState{
			.lexerState = lexerState,
			.exprs = exprs,
			.symbols = state.symbols,
			.positions = state.positions,
			.basePath = basePath,
			.origin = lexerState.origin,
			.rootFS = state.rootFS,
			.settings = state.settings,
		};

		std::vector<char> pdata;
		yyscanner scanner(&lexerState);
		prepareParseData(input,pdata);

		yy_scan_buffer(pdata.data(), pdata.size(), scanner);
		nix::Parser parser(scanner, &parserState);
		parser.parse();

		result.rootExpression = parserState.result;

		scanner = yyscanner(&lexerState);
		prepareParseData(input, pdata);
		yy_scan_buffer(pdata.data(), pdata.size(), scanner);

		nix::Parser::value_type vty;
		nix::Parser::location_type lty;

		NixToken::kind_utype rval;
		while((rval = yylex(&vty,&lty,scanner,&parserState)) != 0){
			uint32_t begin = lty.beginOffset;
			uint32_t end = lty.endOffset;
#ifndef NDEBUG
			result.tokens.push_back(NixToken{ rval, begin, end, input.substr(begin, end - begin) });
#else
			result.tokens.push_back(NixToken{ rval, begin, end});
#endif
		}
	}
}

}