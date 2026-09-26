
#include "altparse.h"

#include <nix/expr/eval.hh>
#include <nix/expr/nixexpr.hh>
#include <nix/expr/parser-state.hh>
#include <nix/expr/symbol-table.hh>
#include <nix/lexer-tab.hh>
#include <nix/parser-tab.hh>
#include <optional>
#include <string_view>
#include <utility>

#include "steal.h"

EXPORT_PRIVATE_MEMBER(positionToDocComment, &nix::EvalState::positionToDocComment);

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
			yylex_destroy(&payload.value());
			payload.reset();
		}
	}
	~yyscanner(){ reset(); }
	operator yyscan_t(){ return payload.value(); }
};

ParseResult parseExprFromString(nix::EvalState& state, const nix::SourcePath& path, nix::Exprs& exprs){

	ParseResult result{};
	result.sourceString = path.resolveSymlinks().readFile();

	nix::Pos::Origin origin{path};
	nix::SourcePath basePath = path.parent();

	auto [it, _] = (state.*positionToDocComment).try_emplace(path);
	nix::DocCommentMap* docComments = &it->second;

	{
		nix::LexerState lexerState{
			.positionToDocComment = *docComments,
			.positions = state.positions,
			.origin = state.positions.addOrigin(origin, result.sourceString.length() + 2),
		};
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
		prepareParseData(result.sourceString,pdata);

		yy_scan_buffer(pdata.data(), pdata.size(), scanner);
		nix::Parser parser(scanner, &parserState);
		parser.parse();

		result.rootExpression = parserState.result;

		scanner = yyscanner(&lexerState);
		prepareParseData(result.sourceString, pdata);
		yy_scan_buffer(pdata.data(), pdata.size(), scanner);

		nix::Parser::value_type vty;
		nix::Parser::location_type lty;

		NixToken::kind_utype rval;
		while((rval = yylex(&vty,&lty,scanner,&parserState)) != 0){
			char* text = yyget_text(scanner);
			int len = yyget_leng(scanner);
			YYSTYPE sty = yyget_lval(&scanner);
			uint32_t begin = text - pdata.data();
			uint32_t end = begin + len;
			/*nix::Symbol symbol{};
			switch(rval){
				case NixToken::kind_type::STR:
				case NixToken::kind_type::ID:
					symbol = state.symbols.create(sty.as<nix::StringToken>());
					break;
				case NixToken::kind_type::IF:
				case NixToken::kind_type::THEN:
				case NixToken::kind_type::ELSE:
				case NixToken::kind_type::ASSERT:
				case NixToken::kind_type::WITH:
				case NixToken::kind_type::LET:
				case NixToken::kind_type::IN_KW:
				case NixToken::kind_type::REC:
				case NixToken::kind_type::INHERIT:
				case NixToken::kind_type::OR:
					symbol = state.symbols.create(std::string_view(result.sourceString).substr(begin, end - begin));
					break;
			}*/
			result.tokens.push_back(NixToken{ rval, begin, end});
		}
	}

	return result;
}