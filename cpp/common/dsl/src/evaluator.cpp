// Evaluator: walks an AST and produces a Value against a Context.
//
// The evaluator is a small recursive visitor — each AST node kind has one
// case in the visit. We intentionally keep this in one file so the inliner
// can see across cases, and to make the sandbox boundary easy to audit:
// only the operations enumerated here are ever performed.

#include "nl_router/dsl/dsl.hpp"
#include "nl_router/dsl/ast.hpp"
#include "nl_router/dsl/context.hpp"
#include "nl_router/dsl/errors.hpp"
#include "nl_router/dsl/value.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace nl_router::dsl {

// Need a definition for value.cpp's symbols — declared earlier in its own
// translation unit. We forward-declare here as needed for linking only;
// nothing here actually references value.cpp's body.

// ---- Forward declaration ----

static Value eval_node(const Expr& e, const Context& ctx);

// ---- Helpers ------------------------------------------------------------

namespace {

// Allowed builtin function names. Anything outside this set throws at eval
// time so operators get a clear error rather than silent surprise.
const std::unordered_set<std::string> kBuiltinFns = {
    "int", "float", "str", "bool", "len",
    "abs", "min", "max", "round",
    // (sum, pow, chr, ord deferred — throw NotImplementedError below)
};

// Allowed string methods (on Value of kind str).
const std::unordered_set<std::string> kStringMethods = {
    "lower", "upper", "strip", "startswith", "endswith", "contains",
};

std::string str_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string str_upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string str_strip(std::string s) {
    auto is_ws = [](unsigned char c){ return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

// Python-style int() on a string: handle optional leading "+" or "-".
std::int64_t str_to_int(const std::string& s) {
    try {
        std::size_t pos = 0;
        const auto v = std::stoll(s, &pos);
        // Reject trailing non-whitespace junk to be strict.
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) {
            throw TypeError("int(): cannot parse '" + s + "' as int");
        }
        return v;
    } catch (const std::invalid_argument&) {
        throw TypeError("int(): cannot parse '" + s + "' as int");
    } catch (const std::out_of_range&) {
        throw TypeError("int(): value out of range: " + s);
    }
}

double str_to_double(const std::string& s) {
    try {
        std::size_t pos = 0;
        const auto v = std::stod(s, &pos);
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos != s.size()) {
            throw TypeError("float(): cannot parse '" + s + "' as float");
        }
        return v;
    } catch (const std::invalid_argument&) {
        throw TypeError("float(): cannot parse '" + s + "' as float");
    } catch (const std::out_of_range&) {
        throw TypeError("float(): value out of range: " + s);
    }
}

// Length of a Value: strings count chars (bytes — DICOM is ASCII or UTF-8
// where multi-byte chars are rare in routing predicates); lists count items.
std::int64_t value_len(const Value& v) {
    if (v.is_string()) return static_cast<std::int64_t>(v.as_string().size());
    if (v.is_list())   return static_cast<std::int64_t>(v.as_list().size());
    throw TypeError(std::string{"len(): unsupported type "} + v.type_name());
}

// `in` operator. Supported combinations:
//   * "needle" in "haystack"  -> substring match
//   * needle   in (a, b, c)   -> any element matches via python_eq
bool in_operator(const Value& needle, const Value& haystack) {
    if (haystack.is_string()) {
        if (!needle.is_string()) {
            throw TypeError("`in str` requires str on the left");
        }
        return haystack.as_string().find(needle.as_string()) != std::string::npos;
    }
    if (haystack.is_list()) {
        for (const auto& item : haystack.as_list()) {
            if (needle.python_eq(item)) return true;
        }
        return false;
    }
    throw TypeError(std::string{"`in` requires str or tuple on the right, got "} +
                    haystack.type_name());
}

// Call a builtin function. Pre-evaluated args; we don't lazy-evaluate.
Value call_builtin(const std::string& name, const std::vector<Value>& args) {
    auto require_arity = [&](std::size_t n) {
        if (args.size() != n) {
            throw TypeError(name + "(): expected " + std::to_string(n) +
                            " argument(s), got " + std::to_string(args.size()));
        }
    };

    if (name == "int") {
        require_arity(1);
        const auto& a = args.front();
        if (a.is_int())    return a;
        if (a.is_bool())   return Value{static_cast<std::int64_t>(a.as_bool() ? 1 : 0)};
        if (a.is_float())  return Value{static_cast<std::int64_t>(a.as_float())};
        if (a.is_string()) return Value{str_to_int(a.as_string())};
        throw TypeError(std::string{"int(): unsupported type "} + a.type_name());
    }
    if (name == "float") {
        require_arity(1);
        const auto& a = args.front();
        if (a.is_float())  return a;
        if (a.is_int())    return Value{static_cast<double>(a.as_int())};
        if (a.is_bool())   return Value{a.as_bool() ? 1.0 : 0.0};
        if (a.is_string()) return Value{str_to_double(a.as_string())};
        throw TypeError(std::string{"float(): unsupported type "} + a.type_name());
    }
    if (name == "str") {
        require_arity(1);
        const auto& a = args.front();
        if (a.is_string()) return a;
        return Value{to_string(a)};
    }
    if (name == "bool") {
        require_arity(1);
        return Value{args.front().truthy()};
    }
    if (name == "len") {
        require_arity(1);
        return Value{value_len(args.front())};
    }
    if (name == "abs") {
        require_arity(1);
        const auto& a = args.front();
        if (a.is_int())   return Value{std::abs(a.as_int())};
        if (a.is_float()) return Value{std::fabs(a.as_float())};
        throw TypeError(std::string{"abs(): unsupported type "} + a.type_name());
    }
    if (name == "min" || name == "max") {
        if (args.empty()) {
            throw TypeError(name + "(): expected at least 1 argument");
        }
        // Pythonic semantics: min(iter) or min(a, b, c, ...). If a single
        // list arg, walk its elements; otherwise compare the args themselves.
        const std::vector<Value>* items = nullptr;
        std::vector<Value> tmp;
        if (args.size() == 1 && args.front().is_list()) {
            items = &args.front().as_list();
        } else {
            tmp = args;
            items = &tmp;
        }
        if (items->empty()) {
            throw TypeError(name + "(): empty sequence");
        }
        std::size_t best_i = 0;
        for (std::size_t i = 1; i < items->size(); ++i) {
            const int cmp = (*items)[i].python_cmp((*items)[best_i]);
            if (name == "min" ? cmp < 0 : cmp > 0) {
                best_i = i;
            }
        }
        return (*items)[best_i];
    }
    if (name == "round") {
        // round(x) → int; round(x, n) → float to n decimals.
        if (args.empty() || args.size() > 2) {
            throw TypeError("round(): expected 1 or 2 arguments");
        }
        const double x = args.front().to_double();
        if (args.size() == 1) {
            // Banker's rounding to nearest integer.
            return Value{static_cast<std::int64_t>(std::llround(x))};
        }
        const std::int64_t n = args[1].as_int();
        const double mult = std::pow(10.0, static_cast<double>(n));
        return Value{std::round(x * mult) / mult};
    }
    // Known-but-deferred:
    if (name == "sum" || name == "pow" || name == "chr" || name == "ord") {
        throw NotImplementedError("builtin " + name + "() not yet implemented");
    }
    throw NotImplementedError("unknown function: " + name);
}

// Call a string method on a string Value.
Value call_string_method(const Value& receiver, const std::string& method,
                          const std::vector<Value>& args) {
    if (!receiver.is_string()) {
        throw TypeError("method '" + method + "' requires a string receiver, got " +
                        receiver.type_name());
    }
    const std::string& s = receiver.as_string();

    auto require_arity = [&](std::size_t n) {
        if (args.size() != n) {
            throw TypeError("str." + method + "(): expected " +
                            std::to_string(n) + " argument(s)");
        }
    };

    if (method == "lower")   { require_arity(0); return Value{str_lower(s)}; }
    if (method == "upper")   { require_arity(0); return Value{str_upper(s)}; }
    if (method == "strip")   { require_arity(0); return Value{str_strip(s)}; }

    if (method == "startswith") {
        require_arity(1);
        if (!args.front().is_string()) {
            throw TypeError("str.startswith(): argument must be a string");
        }
        const auto& prefix = args.front().as_string();
        return Value{s.size() >= prefix.size() &&
                     s.compare(0, prefix.size(), prefix) == 0};
    }
    if (method == "endswith") {
        require_arity(1);
        if (!args.front().is_string()) {
            throw TypeError("str.endswith(): argument must be a string");
        }
        const auto& suffix = args.front().as_string();
        return Value{s.size() >= suffix.size() &&
                     s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0};
    }
    if (method == "contains") {
        // Non-Python convenience method; equivalent to "x" in s but lets
        // operators write tags.SeriesDescription.contains("CINE").
        require_arity(1);
        if (!args.front().is_string()) {
            throw TypeError("str.contains(): argument must be a string");
        }
        return Value{s.find(args.front().as_string()) != std::string::npos};
    }
    throw NotImplementedError("unknown method: str." + method);
}

}  // namespace

// ---- Top-level visitor --------------------------------------------------

static Value eval_node(const Expr& e, const Context& ctx) {
    return std::visit(
        [&](const auto& node) -> Value {
            using T = std::decay_t<decltype(node)>;

            // ---- Literal ----
            if constexpr (std::is_same_v<T, LiteralNode>) {
                return node.value;
            }

            // ---- Field (tags.X / tags["X"]) ----
            else if constexpr (std::is_same_v<T, FieldNode>) {
                auto v = ctx.get(node.name);
                if (!v.has_value()) {
                    // dicomdiablo's behavior: missing tags are empty strings.
                    // Replicate that so rules don't blow up on every missing
                    // optional tag.
                    return Value{std::string{}};
                }
                return Value{*v};
            }

            // ---- List / tuple literal ----
            else if constexpr (std::is_same_v<T, ListNode>) {
                ValueList out;
                out.reserve(node.items.size());
                for (const auto& item : node.items) {
                    out.push_back(eval_node(*item, ctx));
                }
                return Value{std::move(out)};
            }

            // ---- Unary ----
            else if constexpr (std::is_same_v<T, UnaryNode>) {
                auto operand = eval_node(*node.operand, ctx);
                switch (node.op) {
                    case UnOp::LogicalNot:
                        return Value{!operand.truthy()};
                    case UnOp::Negate:
                        if (operand.is_int())   return Value{-operand.as_int()};
                        if (operand.is_float()) return Value{-operand.as_float()};
                        throw TypeError(std::string{"unary '-' on "} +
                                        operand.type_name());
                }
                throw InternalError("unreachable unary op");
            }

            // ---- Binary ----
            else if constexpr (std::is_same_v<T, BinaryNode>) {
                // Short-circuit boolean ops without evaluating rhs unnecessarily.
                if (node.op == BinOp::LogicalAnd) {
                    const auto lhs = eval_node(*node.lhs, ctx);
                    if (!lhs.truthy()) return Value{false};
                    return Value{eval_node(*node.rhs, ctx).truthy()};
                }
                if (node.op == BinOp::LogicalOr) {
                    const auto lhs = eval_node(*node.lhs, ctx);
                    if (lhs.truthy()) return Value{true};
                    return Value{eval_node(*node.rhs, ctx).truthy()};
                }

                const auto lhs = eval_node(*node.lhs, ctx);
                const auto rhs = eval_node(*node.rhs, ctx);

                switch (node.op) {
                    case BinOp::Eq:     return Value{lhs.python_eq(rhs)};
                    case BinOp::NotEq:  return Value{!lhs.python_eq(rhs)};
                    case BinOp::Lt:     return Value{lhs.python_cmp(rhs) <  0};
                    case BinOp::LtEq:   return Value{lhs.python_cmp(rhs) <= 0};
                    case BinOp::Gt:     return Value{lhs.python_cmp(rhs) >  0};
                    case BinOp::GtEq:   return Value{lhs.python_cmp(rhs) >= 0};
                    case BinOp::In:     return Value{in_operator(lhs, rhs)};
                    default: break;
                }
                throw InternalError("unreachable binary op");
            }

            // ---- Builtin call ----
            else if constexpr (std::is_same_v<T, CallNode>) {
                std::vector<Value> args;
                args.reserve(node.args.size());
                for (const auto& a : node.args) {
                    args.push_back(eval_node(*a, ctx));
                }
                return call_builtin(node.name, args);
            }

            // ---- Method call ----
            else if constexpr (std::is_same_v<T, MethodCallNode>) {
                auto receiver = eval_node(*node.target, ctx);
                std::vector<Value> args;
                args.reserve(node.args.size());
                for (const auto& a : node.args) {
                    args.push_back(eval_node(*a, ctx));
                }
                return call_string_method(receiver, node.method, args);
            }
        },
        e.node());
}

// ---- Public API ---------------------------------------------------------

Value eval(const Expr& root, const Context& ctx) {
    return eval_node(root, ctx);
}

bool evaluate(const Expr& root, const Context& ctx) {
    return eval_node(root, ctx).truthy();
}

bool evaluate(std::string_view source, const Context& ctx) {
    auto ast = parse(source);
    return evaluate(*ast, ctx);
}

}  // namespace nl_router::dsl
