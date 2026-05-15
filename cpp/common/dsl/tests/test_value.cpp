// Tests for the Value type and its Python-style coercions.

#include "nl_router/dsl/value.hpp"
#include "nl_router/dsl/errors.hpp"

#include <catch2/catch_test_macros.hpp>

using nl_router::dsl::Value;
using nl_router::dsl::ValueList;
using nl_router::dsl::TypeError;

TEST_CASE("Value type discriminants", "[value]") {
    REQUIRE(Value{}.is_null());
    REQUIRE(Value{true}.is_bool());
    REQUIRE(Value{static_cast<std::int64_t>(7)}.is_int());
    REQUIRE(Value{3.14}.is_float());
    REQUIRE(Value{"hello"}.is_string());
    REQUIRE(Value{ValueList{Value{1}, Value{2}}}.is_list());
}

TEST_CASE("Value truthiness matches Python", "[value]") {
    REQUIRE_FALSE(Value{}.truthy());
    REQUIRE_FALSE(Value{false}.truthy());
    REQUIRE_FALSE(Value{static_cast<std::int64_t>(0)}.truthy());
    REQUIRE_FALSE(Value{0.0}.truthy());
    REQUIRE_FALSE(Value{""}.truthy());
    REQUIRE_FALSE(Value{ValueList{}}.truthy());

    REQUIRE(Value{true}.truthy());
    REQUIRE(Value{static_cast<std::int64_t>(1)}.truthy());
    REQUIRE(Value{0.5}.truthy());
    REQUIRE(Value{"x"}.truthy());
    REQUIRE(Value{ValueList{Value{1}}}.truthy());
}

TEST_CASE("Value equality across numeric types", "[value]") {
    // Mixed-type numeric eq should match Python: 1 == 1.0 -> True.
    REQUIRE(Value{static_cast<std::int64_t>(1)}.python_eq(Value{1.0}));
    REQUIRE(Value{true}.python_eq(Value{static_cast<std::int64_t>(1)}));
    REQUIRE(Value{false}.python_eq(Value{0.0}));

    // Cross-kind strings/numbers should be unequal.
    REQUIRE_FALSE(Value{"1"}.python_eq(Value{static_cast<std::int64_t>(1)}));
    REQUIRE_FALSE(Value{"hello"}.python_eq(Value{ValueList{}}));

    // String/string and list/list element-wise.
    REQUIRE(Value{"x"}.python_eq(Value{"x"}));
    REQUIRE_FALSE(Value{"x"}.python_eq(Value{"X"}));
    REQUIRE(Value{ValueList{Value{1}, Value{"a"}}}
                .python_eq(Value{ValueList{Value{1}, Value{"a"}}}));
    REQUIRE_FALSE(Value{ValueList{Value{1}}}
                .python_eq(Value{ValueList{Value{1}, Value{"a"}}}));
}

TEST_CASE("Value ordered comparison", "[value]") {
    REQUIRE(Value{static_cast<std::int64_t>(1)}.python_cmp(Value{2.0}) < 0);
    REQUIRE(Value{2.0}.python_cmp(Value{static_cast<std::int64_t>(1)}) > 0);
    REQUIRE(Value{static_cast<std::int64_t>(5)}.python_cmp(Value{static_cast<std::int64_t>(5)}) == 0);

    REQUIRE(Value{"abc"}.python_cmp(Value{"abd"}) < 0);
    REQUIRE(Value{"abc"}.python_cmp(Value{"abc"}) == 0);

    // Cross-kind comparison: TypeError.
    REQUIRE_THROWS_AS(Value{"abc"}.python_cmp(Value{static_cast<std::int64_t>(1)}), TypeError);
    REQUIRE_THROWS_AS(Value{ValueList{}}.python_cmp(Value{static_cast<std::int64_t>(1)}), TypeError);
}

TEST_CASE("Value typed accessors throw on wrong type", "[value]") {
    REQUIRE_THROWS_AS(Value{"x"}.as_int(), TypeError);
    REQUIRE_THROWS_AS(Value{static_cast<std::int64_t>(1)}.as_string(), TypeError);
    REQUIRE_THROWS_AS(Value{}.as_bool(), TypeError);
}

TEST_CASE("Value::to_double", "[value]") {
    REQUIRE(Value{static_cast<std::int64_t>(3)}.to_double() == 3.0);
    REQUIRE(Value{2.5}.to_double() == 2.5);
    REQUIRE(Value{true}.to_double() == 1.0);
    REQUIRE_THROWS_AS(Value{"x"}.to_double(), TypeError);
    REQUIRE_THROWS_AS(Value{ValueList{}}.to_double(), TypeError);
}
