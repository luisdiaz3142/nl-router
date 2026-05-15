// nl_router/dsl/ast.hpp
//
// Abstract syntax tree node types for the rule DSL.
//
// AST is owned by std::unique_ptr<Expr>. Each node carries enough source
// position info to point at the offending location in error messages.
//
// The tree is small (≤14 node kinds); we use a tagged enum + std::variant
// over the per-kind data rather than a polymorphic class hierarchy. This
// keeps the evaluator's recursion path predictable for branch prediction.
#pragma once

#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "value.hpp"

namespace nl_router::dsl {

class Expr;
using ExprPtr = std::unique_ptr<Expr>;

// Binary operator opcodes. The set is closed at compile time; the grammar
// is the only thing that emits these.
enum class BinOp {
    // boolean
    LogicalAnd,
    LogicalOr,
    // comparison
    Eq,
    NotEq,
    Lt,
    LtEq,
    Gt,
    GtEq,
    // membership
    In,
};

// Unary operator opcodes.
enum class UnOp {
    LogicalNot,
    Negate,    // -x
};

// AST node kinds. Mirrors the grammar's top-level forms.
struct LiteralNode { Value value; };
struct FieldNode   { std::string name; };     // tags.X  or  tags["X"]
struct ListNode    { std::vector<ExprPtr> items; };
struct UnaryNode   { UnOp op; ExprPtr operand; };
struct BinaryNode  { BinOp op; ExprPtr lhs; ExprPtr rhs; };
struct CallNode    { std::string name; std::vector<ExprPtr> args; };   // float(x), len(x), ...
struct MethodCallNode {
    ExprPtr      target;     // .lower() / .startswith("X") receiver
    std::string  method;     // method name
    std::vector<ExprPtr> args;
};

class Expr {
public:
    using Variant = std::variant<
        LiteralNode,
        FieldNode,
        ListNode,
        UnaryNode,
        BinaryNode,
        CallNode,
        MethodCallNode
    >;

    Expr(Variant v, std::size_t line, std::size_t column)
        : v_{std::move(v)}, line_{line}, column_{column} {}

    template <class T>
    Expr(T&& node, std::size_t line, std::size_t column)
        : v_{std::forward<T>(node)}, line_{line}, column_{column} {}

    const Variant& node()   const noexcept { return v_; }
    std::size_t    line()   const noexcept { return line_; }
    std::size_t    column() const noexcept { return column_; }

    // Convenience: type-checked accessors. Throws std::bad_variant_access if wrong.
    template <class T> const T& as() const { return std::get<T>(v_); }
    template <class T>       T& as()       { return std::get<T>(v_); }

    template <class T> bool is() const noexcept {
        return std::holds_alternative<T>(v_);
    }

private:
    Variant      v_;
    std::size_t  line_   {0};
    std::size_t  column_ {0};
};

// Convenience factory — keeps source positions consistent.
template <class T>
ExprPtr make_expr(T&& node, std::size_t line, std::size_t column) {
    return std::make_unique<Expr>(std::forward<T>(node), line, column);
}

}  // namespace nl_router::dsl
