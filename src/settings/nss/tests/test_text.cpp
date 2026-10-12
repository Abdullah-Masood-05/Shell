#include "TestUtil.h"

using namespace nss;

TEST_CASE("decode: UTF-8 with and without BOM")
{
	auto d = decode("\xEF\xBB\xBFtheme{}\r\n");
	REQUIRE(d.ok);
	CHECK(d.text == "theme{}\r\n");
	CHECK(d.format.encoding == Encoding::Utf8);
	CHECK(d.format.bom);
	CHECK(d.format.eol == Eol::CRLF);
	CHECK(d.lossless);

	auto e = decode("a=\"\xC3\xA9\"\n");
	REQUIRE(e.ok);
	CHECK(e.format.encoding == Encoding::Utf8);
	CHECK_FALSE(e.format.bom);
	CHECK(e.format.eol == Eol::LF);
	CHECK(encode(e.text, e.format) == "a=\"\xC3\xA9\"\n");
}

TEST_CASE("decode: UTF-16 LE/BE with BOM, LE without BOM")
{
	std::string le("\xFF\xFE" "a\0=\0\x01\x26\r\0\n\0", 12);  // a=U+2601 CRLF
	auto d = decode(le);
	REQUIRE(d.ok);
	CHECK(d.format.encoding == Encoding::Utf16LE);
	CHECK(d.format.bom);
	CHECK(d.text == "a=\xE2\x98\x81\r\n");
	CHECK(encode(d.text, d.format) == le);

	std::string be("\xFE\xFF\0a\0=\0b", 8);
	auto b = decode(be);
	REQUIRE(b.ok);
	CHECK(b.format.encoding == Encoding::Utf16BE);
	CHECK(b.text == "a=b");
	CHECK(encode(b.text, b.format) == be);

	std::string nobom("a\0b\0", 4);
	auto n = decode(nobom);
	REQUIRE(n.ok);
	CHECK(n.format.encoding == Encoding::Utf16LE);
	CHECK_FALSE(n.format.bom);
	CHECK(n.text == "ab");

	// A lone surrogate survives the round trip.
	std::string lone("\xFF\xFE\x00\xD8x\0", 6);
	auto l = decode(lone);
	REQUIRE(l.ok);
	CHECK(l.lossless);
	CHECK(encode(l.text, l.format) == lone);
}

TEST_CASE("decode: ANSI fallback and refused encodings")
{
	auto d = decode("x=\"caf\xE9\"\r\n");
	REQUIRE(d.ok);
	CHECK(d.format.encoding == Encoding::Ansi);
	if(::GetACP() == 1252)
	{
		CHECK(d.text == "x=\"caf\xC3\xA9\"\r\n");
		CHECK(d.lossless);
	}
	CHECK(encode(d.text, d.format) == "x=\"caf\xE9\"\r\n");

	CHECK_FALSE(decode(std::string("\0\0\xFE\xFF", 4)).ok);
	CHECK_FALSE(decode("+/v8 text").ok);
}

TEST_CASE("eol detection")
{
	bool mixed = false;
	CHECK(detectEol("a\r\nb\r\nc\n", &mixed) == Eol::CRLF);
	CHECK(mixed);
	CHECK(detectEol("a\nb\n", &mixed) == Eol::LF);
	CHECK_FALSE(mixed);
	CHECK(detectEol("a\rb\r") == Eol::CR);
	CHECK(detectEol("no breaks") == Eol::CRLF);
}
