// nl_router/dsl/dsl.hpp
//
// Public API of the rule expression DSL.
//
// Typical usage:
//
//     auto ast = dsl::parse(R"(tags.Modality == "CT" and float(tags.SliceThickness) < 2.0)");
//     dsl::Context ctx;
//     ctx.set("Modality", "CT");
//     ctx.set("SliceThickness", "1.5");   // DICOM DS is a string
//     bool result = dsl::evaluate(*ast, ctx);   // → true
//
// `parse()` is cheap to do repeatedly but typically operators cache the AST
// per (rule_id, predicate_hash) in the Router. `evaluate()` runs every time
// a work_queue row needs routing.
#pragma once

#include <string_view>

#include "ast.hpp"
#include "context.hpp"
#include "errors.hpp"
#include "value.hpp"

namespace nl_router::dsl {

// Parse a rule expression. Throws ParseError on failure. The returned AST is
// safe to evaluate any number of times against any number of contexts.
ExprPtr parse(std::string_view source);

// Evaluate a previously-parsed AST against a context.
// Returns the resulting Value (typically a bool, but the grammar allows any
// expression at the root — caller decides whether to require boolean).
Value eval(const Expr& root, const Context& ctx);

// Convenience: evaluate and return the Python-truthiness of the result as a
// bool. Equivalent to `eval(...).truthy()` but reads better at call sites.
bool evaluate(const Expr& root, const Context& ctx);

// Convenience: parse and evaluate in one step. Mostly for tests and the
// management API's "validate this predicate" endpoint.
bool evaluate(std::string_view source, const Context& ctx);

}  // namespace nl_router::dsl
