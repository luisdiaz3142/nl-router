// nl_router/dsl/value.hpp
//
// Value type used by the rule expression DSL.
//
// Modeled to match Python's eval() semantics so that rules from
// dicomdiablo (which uses Python eval) paste in verbatim:
//
//     "CINE" in tags.SeriesDescription.lower()
//     float(tags.SliceThickness) < 2.0
//     tags.StationName in ("CT01","CT02")
//
// A Value is one of:
//   * Null                  - no value (e.g. JSONB key missing)
//   * Bool                  - True / False
//   * Int   (int64_t)       - integer literal
//   * Float (double)        - float literal or float() cast
//   * String                - DICOM tag values arrive as strings
//   * List<Value>           - tuple literals, function results
//
// We intentionally avoid bringing in std::optional<Value> or smart-pointer
// values — the variant carries everything inline, which keeps the evaluator's
// inner loop allocation-free for the scalar cases.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <variant>
#include <vector>

namespace nl_router::dsl {

class Value;
using ValueList = std::vector<Value>;

class Value {
public:
    // Tagged variant. The discriminant order matters for std::visit, but is
    // otherwise an implementation detail.
    using Variant = std::variant<
        std::monostate,   // 0 - null
        bool,             // 1 - bool
        std::int64_t,     // 2 - int
        double,           // 3 - float
        std::string,      // 4 - string
        ValueList         // 5 - list / tuple
    >;

    // ---- Constructors ----
    Value() noexcept = default;                                       // null
    Value(std::nullptr_t) noexcept : v_{std::monostate{}} {}
    Value(bool b) noexcept : v_{b} {}
    Value(int i) noexcept : v_{static_cast<std::int64_t>(i)} {}
    Value(std::int64_t i) noexcept : v_{i} {}
    Value(double d) noexcept : v_{d} {}
    Value(const char* s) : v_{std::string{s}} {}
    Value(std::string s) : v_{std::move(s)} {}
    Value(ValueList list) : v_{std::move(list)} {}

    // ---- Discriminants ----
    bool is_null()   const noexcept { return v_.index() == 0; }
    bool is_bool()   const noexcept { return v_.index() == 1; }
    bool is_int()    const noexcept { return v_.index() == 2; }
    bool is_float()  const noexcept { return v_.index() == 3; }
    bool is_string() const noexcept { return v_.index() == 4; }
    bool is_list()   const noexcept { return v_.index() == 5; }
    bool is_number() const noexcept { return is_int() || is_float(); }

    // ---- Typed accessors (throw if wrong type) ----
    bool                 as_bool()   const;
    std::int64_t         as_int()    const;
    double               as_float()  const;
    const std::string&   as_string() const;
    const ValueList&     as_list()   const;

    // Convert to a double for arithmetic / comparison. Throws if not numeric.
    double to_double() const;

    // Python-style truthiness: null=false, 0=false, empty=false, otherwise true.
    bool truthy() const noexcept;

    // Discriminant name for error messages ("str", "int", "float", "bool",
    // "list", "null").
    const char* type_name() const noexcept;

    // Equality matches Python rules: numeric cross-type comparisons coerce
    // (1 == 1.0 is true); strings never equal numbers.
    bool python_eq(const Value& other) const;

    // Ordered comparison. Throws TypeError for incompatible types. Strings
    // compare lexicographically; numbers compare arithmetically.
    int python_cmp(const Value& other) const;

    // Underlying variant access for code that wants std::visit.
    const Variant& raw() const noexcept { return v_; }
    Variant&       raw()       noexcept { return v_; }

private:
    Variant v_{std::monostate{}};
};

// For debug / error messages and test failure diagnostics.
std::ostream& operator<<(std::ostream& os, const Value& v);
std::string to_string(const Value& v);

}  // namespace nl_router::dsl
