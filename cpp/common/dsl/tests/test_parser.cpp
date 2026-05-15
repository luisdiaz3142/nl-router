// Tests for the DSL parser. Focus on accepting valid rules and rejecting
// invalid ones cleanly; deeper grammar coverage runs through the evaluator
// tests where the AST is also exercised end-to-end.

#include "nl_router/dsl/dsl.hpp"

#include <catch2/catch_test_macros.hpp>

using nl_router::dsl::parse;
using nl_router::dsl::ParseError;

TEST_CASE("Parser accepts the dicomdiablo grammar surface", "[parser]") {
    REQUIRE_NOTHROW(parse("True"));
    REQUIRE_NOTHROW(parse("False"));
    REQUIRE_NOTHROW(parse("42"));
    REQUIRE_NOTHROW(parse("3.14"));
    REQUIRE_NOTHROW(parse(R"("hello")"));
    REQUIRE_NOTHROW(parse("'hello'"));
    REQUIRE_NOTHROW(parse("tags.Modality"));
    REQUIRE_NOTHROW(parse(R"(tags["Modality"])"));
    REQUIRE_NOTHROW(parse("tags.Modality == \"CT\""));
    REQUIRE_NOTHROW(parse("tags.Modality != \"CT\""));
    REQUIRE_NOTHROW(parse("tags.SliceThickness < 2.0"));
    REQUIRE_NOTHROW(parse("tags.SliceThickness >= 2.0"));
    REQUIRE_NOTHROW(parse("not (tags.Modality == \"CT\")"));
    REQUIRE_NOTHROW(parse(R"(tags.Modality == "CT" and tags.SenderAET == "MOD1")"));
    REQUIRE_NOTHROW(parse(R"(tags.Modality == "CT" or tags.Modality == "MR")"));
    REQUIRE_NOTHROW(parse(R"(tags.StationName in ("CT01","CT02"))"));
    REQUIRE_NOTHROW(parse(R"("CINE" in tags.SeriesDescription.lower())"));
    REQUIRE_NOTHROW(parse("float(tags.SliceThickness) < 2.0"));
    REQUIRE_NOTHROW(parse("len(tags.SeriesDescription) > 0"));
    REQUIRE_NOTHROW(parse("max(1, 2, 3) == 3"));
    REQUIRE_NOTHROW(parse("abs(-7) == 7"));
    REQUIRE_NOTHROW(parse("tags.SeriesDescription.startswith(\"AX\")"));
    REQUIRE_NOTHROW(parse("tags.SeriesDescription.endswith(\"_pre\")"));
}

TEST_CASE("Parser whitespace and parens", "[parser]") {
    REQUIRE_NOTHROW(parse("  tags.Modality  ==  \"CT\"  "));
    REQUIRE_NOTHROW(parse("(((tags.Modality == \"CT\")))"));
    REQUIRE_NOTHROW(parse("(tags.A == 1 and tags.B == 2) or tags.C == 3"));
}

TEST_CASE("Parser rejects malformed input", "[parser]") {
    // Unterminated string.
    REQUIRE_THROWS_AS(parse(R"(tags.X == "unterminated)"), ParseError);
    // Missing right-hand side.
    REQUIRE_THROWS_AS(parse("tags.X =="), ParseError);
    // Bare operator.
    REQUIRE_THROWS_AS(parse("=="), ParseError);
    // Unbalanced parens.
    REQUIRE_THROWS_AS(parse("(tags.X == 1"), ParseError);
    REQUIRE_THROWS_AS(parse("tags.X == 1)"), ParseError);
    // Garbage trailing tokens.
    REQUIRE_THROWS_AS(parse("tags.X == 1 garbage"), ParseError);
    // Comp chaining isn't allowed.
    REQUIRE_THROWS_AS(parse("1 < 2 < 3"), ParseError);
}

TEST_CASE("Parse error carries line/column", "[parser][errors]") {
    try {
        parse("tags.X ===");
        FAIL("expected ParseError");
    } catch (const ParseError& e) {
        // Don't assert exact position — PEGTL's error reporting can point at
        // various spots depending on where backtracking gave up — just ensure
        // we get plausible 1-based positions.
        REQUIRE(e.line() >= 1);
        REQUIRE(e.column() >= 1);
    }
}
