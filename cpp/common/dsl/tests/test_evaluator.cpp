// End-to-end tests for parse + evaluate.
//
// These exercise the same predicates a real dicomdiablo deployment uses, so
// each test doubles as a compatibility check: the expression strings come
// from real-world routing rules.

#include "nl_router/dsl/dsl.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace nl_router::dsl;

// Build a Context populated with a few canonical tags.
static Context default_ctx() {
    Context c;
    c.set("Modality", "CT");
    c.set("SenderAET", "MODALITY1");
    c.set("ReceiverAET", "NL_ROUTER");
    c.set("CallingAET", "MODALITY1");        // alt name some tools use
    c.set("StationName", "CT01");
    c.set("StudyDescription", "CHEST WITH CONTRAST");
    c.set("SeriesDescription", "AX 2.0 MM CINE");
    c.set("SliceThickness", "1.5");          // DICOM DS arrives as string
    c.set("BodyPartExamined", "CHEST");
    c.set("PatientID", "ABC123");
    return c;
}

TEST_CASE("Literal-only predicates", "[eval][literals]") {
    Context c;
    REQUIRE(evaluate("True", c));
    REQUIRE_FALSE(evaluate("False", c));
    // Lowercase aliases — M22 added these because operators kept typing
    // `true` / `false` (C / JS / Go habit) and the prior grammar only
    // accepted the Python-style capitalized form. Both must work and
    // produce identical results.
    REQUIRE(evaluate("true", c));
    REQUIRE_FALSE(evaluate("false", c));
    REQUIRE(evaluate("not false", c));
    REQUIRE_FALSE(evaluate("not true", c));
    REQUIRE(evaluate("true and True", c));
    REQUIRE(evaluate("False or true", c));
    REQUIRE(evaluate("1 == 1", c));
    REQUIRE_FALSE(evaluate("1 == 2", c));
    REQUIRE(evaluate("\"a\" == \"a\"", c));
    REQUIRE_FALSE(evaluate("\"a\" == \"b\"", c));
    REQUIRE(evaluate("1 < 2", c));
    REQUIRE(evaluate("2 <= 2", c));
    REQUIRE(evaluate("3 > 2", c));
    REQUIRE(evaluate("3 >= 3", c));
    REQUIRE(evaluate("not False", c));
    REQUIRE_FALSE(evaluate("not True", c));
}

TEST_CASE("Field access and comparison", "[eval][field]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"(tags.Modality == "CT")", c));
    REQUIRE_FALSE(evaluate(R"(tags.Modality == "MR")", c));
    REQUIRE(evaluate(R"(tags.Modality != "MR")", c));
    REQUIRE(evaluate(R"(tags["Modality"] == "CT")", c));
}

TEST_CASE("Boolean composition", "[eval][bool]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"(tags.Modality == "CT" and tags.SenderAET == "MODALITY1")", c));
    REQUIRE_FALSE(evaluate(R"(tags.Modality == "CT" and tags.SenderAET == "OTHER")", c));
    REQUIRE(evaluate(R"(tags.Modality == "MR" or tags.Modality == "CT")", c));
    REQUIRE(evaluate(R"(not (tags.Modality == "MR"))", c));
}

TEST_CASE("Numeric coercion via float()", "[eval][cast]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"(float(tags.SliceThickness) < 2.0)", c));
    REQUIRE_FALSE(evaluate(R"(float(tags.SliceThickness) > 2.0)", c));
    REQUIRE(evaluate("float(tags.SliceThickness) == 1.5", c));
}

TEST_CASE("Membership in tuple", "[eval][in][tuple]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"(tags.StationName in ("CT01","CT02"))", c));
    REQUIRE_FALSE(evaluate(R"(tags.StationName in ("CT02","CT03"))", c));
    REQUIRE(evaluate(R"(tags.Modality in ("CT","MR","XA"))", c));
}

TEST_CASE("Substring `in` operator on strings", "[eval][in][string]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"("CINE" in tags.SeriesDescription)", c));
    REQUIRE_FALSE(evaluate(R"("HEAD" in tags.SeriesDescription)", c));
}

TEST_CASE("String methods", "[eval][methods]") {
    auto c = default_ctx();
    REQUIRE(evaluate(R"("cine" in tags.SeriesDescription.lower())", c));
    REQUIRE(evaluate(R"(tags.SeriesDescription.upper() == "AX 2.0 MM CINE")", c));
    REQUIRE(evaluate(R"(tags.SeriesDescription.startswith("AX"))", c));
    REQUIRE(evaluate(R"(tags.SeriesDescription.endswith("CINE"))", c));
    REQUIRE_FALSE(evaluate(R"(tags.SeriesDescription.startswith("SAG"))", c));
}

TEST_CASE("Builtins: int, float, str, bool, len, abs, min, max, round",
          "[eval][builtins]") {
    Context c;
    c.set("X", "42");
    c.set("Y", "  hello  ");

    REQUIRE(evaluate("int(tags.X) == 42", c));
    REQUIRE(evaluate("float(tags.X) == 42.0", c));
    REQUIRE(evaluate(R"(str(42) == "42")", c));
    REQUIRE(evaluate("bool(tags.X)", c));
    REQUIRE(evaluate("len(tags.Y) == 9", c));
    REQUIRE(evaluate("abs(-7) == 7", c));
    REQUIRE(evaluate("min(3, 1, 2) == 1", c));
    REQUIRE(evaluate("max(3, 1, 2) == 3", c));
    REQUIRE(evaluate("round(1.4) == 1", c));
    REQUIRE(evaluate("round(1.6) == 2", c));
    REQUIRE(evaluate("round(1.2345, 2) == 1.23", c));
}

TEST_CASE("Operator precedence: and binds tighter than or", "[eval][precedence]") {
    Context c;
    // True or (False and False)  -> True
    REQUIRE(evaluate("True or False and False", c));
    // (True or False) and False would be False; current parse should give True.
    REQUIRE_FALSE(evaluate("False or False and False", c));
}

TEST_CASE("not binds tighter than and / or", "[eval][precedence]") {
    Context c;
    REQUIRE(evaluate("not False and True", c));         // (not False) and True
    REQUIRE_FALSE(evaluate("not True or False", c));    // (not True) or False
}

TEST_CASE("Real-world rule examples from dicomdiablo", "[eval][compat]") {
    auto c = default_ctx();

    REQUIRE(evaluate(
        R"(tags.Modality == "CT" and float(tags.SliceThickness) < 2.0)", c));

    REQUIRE(evaluate(
        R"("cine" in tags.SeriesDescription.lower() and tags.Modality == "CT")", c));

    REQUIRE(evaluate(
        R"(tags.StationName in ("CT01","CT02") and tags.BodyPartExamined == "CHEST")", c));

    REQUIRE_FALSE(evaluate(
        R"(tags.Modality == "MR" and tags.SliceThickness == "3.0")", c));
}

TEST_CASE("Missing tags are treated as empty strings", "[eval][missing]") {
    Context c;
    // Field not set; dicomdiablo-compatible behavior is empty string.
    REQUIRE(evaluate(R"(tags.NoSuchTag == "")", c));
    REQUIRE_FALSE(evaluate(R"(tags.NoSuchTag == "x")", c));
    REQUIRE(evaluate("len(tags.NoSuchTag) == 0", c));
}

TEST_CASE("Aliases let SenderAET resolve to calling_aet", "[eval][aliases]") {
    Context c;
    c.set("calling_aet", "MODALITY1");
    c.set_alias("SenderAET", "calling_aet");
    c.set_alias("ReceiverAET", "called_aet");
    c.set("called_aet", "NL_ROUTER");

    REQUIRE(evaluate(R"(tags.SenderAET == "MODALITY1")", c));
    REQUIRE(evaluate(R"(tags.ReceiverAET == "NL_ROUTER")", c));
}

TEST_CASE("Short-circuit evaluation of and / or", "[eval][short-circuit]") {
    Context c;
    // If `and` short-circuits, the RHS isn't evaluated when LHS is false.
    // We can't easily probe this with builtins, but we can use a tag that
    // would fail numeric coercion if reached.
    c.set("Bad", "not-a-number");

    // False and (would-error)  -> False, with no error.
    REQUIRE_NOTHROW(evaluate("False and float(tags.Bad) < 1.0", c));
    REQUIRE_FALSE(evaluate("False and float(tags.Bad) < 1.0", c));

    // True or (would-error)  -> True, with no error.
    REQUIRE_NOTHROW(evaluate("True or float(tags.Bad) < 1.0", c));
    REQUIRE(evaluate("True or float(tags.Bad) < 1.0", c));
}

TEST_CASE("Type errors at evaluation time", "[eval][errors]") {
    Context c;
    c.set("Bad", "not-a-number");

    REQUIRE_THROWS_AS(evaluate("float(tags.Bad)", c), TypeError);
    REQUIRE_THROWS_AS(evaluate("int(tags.Bad)", c), TypeError);
    // 'in' with wrong RHS type.
    REQUIRE_THROWS_AS(evaluate("1 in 2", c), TypeError);
}

TEST_CASE("Single-quoted strings work too", "[eval][literals]") {
    Context c;
    c.set("Modality", "CT");
    REQUIRE(evaluate(R"(tags.Modality == 'CT')", c));
    REQUIRE(evaluate("'x' == \"x\"", c));
}

TEST_CASE("Unary negation", "[eval][unary]") {
    Context c;
    REQUIRE(evaluate("-1 < 0", c));
    REQUIRE(evaluate("-1.5 < 0", c));
    REQUIRE(evaluate("abs(-7) == 7", c));
}
