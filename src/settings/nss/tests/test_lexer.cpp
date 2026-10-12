#include "TestUtil.h"

using namespace nss;

namespace
{
	std::vector<std::pair<TokenKind, std::string>> lex(std::string_view src)
	{
		std::vector<std::pair<TokenKind, std::string>> out;
		for(auto &t : NssLexer::tokenize(src))
			if(t.kind != TokenKind::Eof)
				out.emplace_back(t.kind, std::string(src.substr(t.begin, t.size())));
		return out;
	}

	std::string rebuild(std::string_view src, const std::vector<Token> &toks)
	{
		std::string out;
		for(auto &t : toks)
			out.append(src.substr(t.triviaBegin, t.end - t.triviaBegin));
		return out;
	}
}

TEST_CASE("lexer is lossless over the shipped configs")
{
	for(auto &file : test::corpus())
	{
		CAPTURE(file.string());
		auto dec = decode(test::readBytes(file));
		REQUIRE(dec.ok);
		std::vector<LexError> errors;
		auto toks = NssLexer::tokenize(dec.text, &errors);
		CHECK(rebuild(dec.text, toks) == dec.text);
		CHECK(errors.empty());
		CHECK(toks.back().kind == TokenKind::Eof);
	}
}

TEST_CASE("lexer token kinds")
{
	auto t = lex("theme.background.opacity = 50 // trailing\n$x=#1e1e1e1e + 0x1F + .5 @ \\uE249 cmd-line");
	REQUIRE(t.size() == 15);
	CHECK(t[0] == std::pair{ TokenKind::Ident, std::string("theme.background.opacity") });
	CHECK(t[1].first == TokenKind::Assign);
	CHECK(t[2] == std::pair{ TokenKind::Number, std::string("50") });
	CHECK(t[3] == std::pair{ TokenKind::Variable, std::string("$x") });
	CHECK(t[4].first == TokenKind::Assign);
	CHECK(t[5] == std::pair{ TokenKind::Color, std::string("#1e1e1e1e") });
	CHECK(t[6].first == TokenKind::Operator);
	CHECK(t[7] == std::pair{ TokenKind::Number, std::string("0x1F") });
	CHECK(t[9] == std::pair{ TokenKind::Number, std::string(".5") });
	CHECK(t[10].first == TokenKind::At);
	CHECK(t[11] == std::pair{ TokenKind::Char, std::string("\\uE249") });
	CHECK(t[12] == std::pair{ TokenKind::Ident, std::string("cmd") });
	CHECK(t[13] == std::pair{ TokenKind::Operator, std::string("-") });
	CHECK(t[14] == std::pair{ TokenKind::Ident, std::string("line") });
}

TEST_CASE("lexer strings")
{
	// "..." takes escapes, including \" and \\.
	std::string str = R"("a \"q\" \\ \x41 \t")";
	auto a = lex("x=" + str + " y");
	REQUIRE(a.size() == 4);
	CHECK(a[2].first == TokenKind::String);
	CHECK(a[2].second == str);

	// '...' has no escapes: a backslash before the quote ends nothing special.
	std::string path = R"('\imports\lang\')";
	auto b = lex("p=" + path + " q");
	REQUIRE(b.size() == 4);
	CHECK(b[2].first == TokenKind::IString);
	CHECK(b[2].second == path);

	// Interpolations may hold nested quotes; comments inside are skipped.
	auto c = lex("cmd='/c @if(sel.type==1,'a','b') /* it's */ x' next");
	REQUIRE(c.size() == 4);
	CHECK(c[2].first == TokenKind::IString);
	CHECK(c[2].second == "'/c @if(sel.type==1,'a','b') /* it's */ x'");
	CHECK(c[3].second == "next");

	// Multi-line single-quoted strings, @@ and @"..." inside.
	auto d = lex("s='line1\r\n@@ @\"q'\" end' t");
	REQUIRE(d.size() == 4);
	CHECK(d[2].second == "'line1\r\n@@ @\"q'\" end'");

	// Backtick strings.
	auto e = lex("arg=`-d \"@sel.path\\.\"`)");
	REQUIRE(e.size() == 4);
	CHECK(e[2].first == TokenKind::IString);
	CHECK(e[3].first == TokenKind::RParen);
}

TEST_CASE("lexer errors are flagged but text is kept")
{
	std::vector<LexError> errors;
	std::string src = "a=\"open\nb=1 /* never closed";
	auto toks = NssLexer::tokenize(src, &errors);
	CHECK(errors.size() == 2);
	CHECK(rebuild(src, toks) == src);
	CHECK(toks[2].error);
}
