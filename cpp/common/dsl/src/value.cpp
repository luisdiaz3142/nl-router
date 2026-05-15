// Implementation of nl_router::dsl::Value.
//
// Most of the work is type coercion to match Python's eval() semantics, since
// dicomdiablo rules paste in verbatim and rely on those rules.

#include "nl_router/dsl/value.hpp"
#include "nl_router/dsl/errors.hpp"

#include <cmath>
#include <ostream>
#include <sstream>
#include <string>
#include <variant>

namespace nl_router::dsl {

// ---- type_name ----------------------------------------------------------

const char* Value::type_name() const noexcept {
    switch (v_.index()) {
        case 0: return "null";
        case 1: return "bool";
        case 2: return "int";
        case 3: return "float";
        case 4: return "str";
        case 5: return "list";
        default: return "?";
    }
}

// ---- typed accessors ----------------------------------------------------

bool Value::as_bool() const {
    if (!is_bool()) throw TypeError(std::string{"expected bool, got "} + type_name());
    return std::get<bool>(v_);
}

std::int64_t Value::as_int() const {
    if (!is_int()) throw TypeError(std::string{"expected int, got "} + type_name());
    return std::get<std::int64_t>(v_);
}

double Value::as_float() const {
    if (!is_float()) throw TypeError(std::string{"expected float, got "} + type_name());
    return std::get<double>(v_);
}

const std::string& Value::as_string() const {
    if (!is_string()) throw TypeError(std::string{"expected str, got "} + type_name());
    return std::get<std::string>(v_);
}

const ValueList& Value::as_list() const {
    if (!is_list()) throw TypeError(std::string{"expected list, got "} + type_name());
    return std::get<ValueList>(v_);
}

// ---- to_double ----------------------------------------------------------

double Value::to_double() const {
    if (is_int())    return static_cast<double>(std::get<std::int64_t>(v_));
    if (is_float())  return std::get<double>(v_);
    if (is_bool())   return std::get<bool>(v_) ? 1.0 : 0.0;
    throw TypeError(std::string{"cannot convert "} + type_name() + " to number");
}

// ---- truthy -------------------------------------------------------------

bool Value::truthy() const noexcept {
    switch (v_.index()) {
        case 0: return false;                                       // null
        case 1: return std::get<bool>(v_);                          // bool
        case 2: return std::get<std::int64_t>(v_) != 0;             // int
        case 3: return std::get<double>(v_) != 0.0;                 // float
        case 4: return !std::get<std::string>(v_).empty();          // str
        case 5: return !std::get<ValueList>(v_).empty();            // list
        default: return false;
    }
}

// ---- equality (Python-like) ---------------------------------------------

bool Value::python_eq(const Value& other) const {
    // null is only equal to null.
    if (is_null() || other.is_null()) return is_null() && other.is_null();

    // bool is a special-case int in Python; but we compare by-type for bool/bool
    // and bool/numeric (coerce to int), never bool/string.
    if (is_bool() && other.is_bool())
        return std::get<bool>(v_) == std::get<bool>(other.v_);

    // Numeric (incl. bool) cross-type comparisons coerce to double.
    const bool a_num = is_number() || is_bool();
    const bool b_num = other.is_number() || other.is_bool();
    if (a_num && b_num) {
        return to_double() == other.to_double();
    }

    // String == string.
    if (is_string() && other.is_string())
        return std::get<std::string>(v_) == std::get<std::string>(other.v_);

    // List == list (element-wise).
    if (is_list() && other.is_list()) {
        const auto& a = std::get<ValueList>(v_);
        const auto& b = std::get<ValueList>(other.v_);
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!a[i].python_eq(b[i])) return false;
        }
        return true;
    }

    // Anything else: unequal. (Python actually raises for some comparisons,
    // but `==` between unlike non-numeric types is just False in CPython,
    // which is what most rule authors expect.)
    return false;
}

// ---- ordered comparison -------------------------------------------------

int Value::python_cmp(const Value& other) const {
    // Numeric (incl. bool) → coerce to double.
    const bool a_num = is_number() || is_bool();
    const bool b_num = other.is_number() || other.is_bool();
    if (a_num && b_num) {
        const double a = to_double();
        const double b = other.to_double();
        if (a < b) return -1;
        if (a > b) return  1;
        return 0;
    }
    // String → lexicographic.
    if (is_string() && other.is_string()) {
        const auto& a = std::get<std::string>(v_);
        const auto& b = std::get<std::string>(other.v_);
        return a.compare(b);
    }
    throw TypeError(
        std::string{"cannot order "} + type_name() + " against " + other.type_name()
    );
}

// ---- streaming / to_string ---------------------------------------------

namespace {
struct StreamVisitor {
    std::ostream& os;
    void operator()(std::monostate)            const { os << "null"; }
    void operator()(bool b)                    const { os << (b ? "True" : "False"); }
    void operator()(std::int64_t i)            const { os << i; }
    void operator()(double d)                  const { os << d; }
    void operator()(const std::string& s)      const { os << '"' << s << '"'; }
    void operator()(const ValueList& l)        const {
        os << "(";
        bool first = true;
        for (const auto& v : l) {
            if (!first) os << ", ";
            first = false;
            os << v;
        }
        os << ")";
    }
};
}  // namespace

std::ostream& operator<<(std::ostream& os, const Value& v) {
    std::visit(StreamVisitor{os}, v.raw());
    return os;
}

std::string to_string(const Value& v) {
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

}  // namespace nl_router::dsl
