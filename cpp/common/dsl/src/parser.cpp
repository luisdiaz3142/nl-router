// Parser implementation: walks the PEGTL grammar and assembles an AST.
//
// Strategy: instead of writing one giant action that constructs a tree on the
// fly, we use a small `state` stack — each rule that produces an expression
// pushes its Expr onto the stack, and combining rules pop their operands
// from the stack. This is the standard PEGTL pattern for arithmetic-style
// expression grammars.

#include "nl_router/dsl/dsl.hpp"
#include "nl_router/dsl/ast.hpp"
#include "nl_router/dsl/errors.hpp"
#include "nl_router/dsl/value.hpp"

#include "grammar.hpp"

#include <tao/pegtl.hpp>
#include <tao/pegtl/contrib/parse_tree.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace nl_router::dsl {

namespace pegtl = tao::pegtl;

namespace {

// ---- Parser state -------------------------------------------------------

// The parser state holds a stack of partially-built AST nodes. Each
// expression-producing rule's action pushes one Expr; combining rules pop
// their operands.
struct State {
    std::vector<ExprPtr> stack;

    // For comp_tail, we need to know which operator was just matched. The
    // grammar's comp_op rule fires before the rhs is built, so we stash the
    // operator here.
    BinOp pending_comp{BinOp::Eq};
    bool  pending_is_in{false};

    // For tuple_or_group, we save the stack size at the opening paren so we
    // can count exactly how many sub-expressions were pushed by the body. We
    // also need to know whether the body had any top-level commas to
    // distinguish `(x)` (grouping) from `(x,)` (1-tuple).
    struct GroupFrame {
        std::size_t stack_size_at_open{0};
    };
    std::vector<GroupFrame> group_stack;

    // Push a built Expr.
    void push(ExprPtr e) { stack.push_back(std::move(e)); }

    // Pop the top Expr.
    ExprPtr pop() {
        if (stack.empty()) {
            throw InternalError("DSL parser: stack underflow (grammar bug)");
        }
        auto e = std::move(stack.back());
        stack.pop_back();
        return e;
    }
};

// ---- Helpers ------------------------------------------------------------

// Decode a Python-style escape sequence character. `c` is the char *after*
// the backslash.
char decode_escape(char c) {
    switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '0':  return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        default:   return c;  // grammar limits possible values; fallback echoes
    }
}

// Strip surrounding quotes and decode escapes from a string-literal token.
std::string decode_string_literal(std::string_view raw) {
    if (raw.size() < 2) return std::string{raw};
    // Strip leading and trailing quote char (already guaranteed by grammar).
    raw.remove_prefix(1);
    raw.remove_suffix(1);

    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            out.push_back(decode_escape(raw[i + 1]));
            ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

}  // namespace

// ---- PEGTL actions ------------------------------------------------------

// Default no-op action (PEGTL requires a template specialization per rule it
// invokes actions on; everything else falls through to nothing).
template <typename Rule>
struct action : pegtl::nothing<Rule> {};

// Source-position helpers — line and column are 1-based in PEGTL.
template <class Input>
inline std::size_t line_of(const Input& in) { return in.position().line; }
template <class Input>
inline std::size_t col_of(const Input& in)  { return in.position().column; }

// -- literals --

template <> struct action<grammar::int_literal> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        const std::string text{in.string()};
        try {
            const auto v = std::stoll(text);
            s.push(make_expr(
                LiteralNode{ Value{static_cast<std::int64_t>(v)} },
                line_of(in), col_of(in)));
        } catch (const std::out_of_range&) {
            throw ParseError("integer literal out of range: " + text,
                             line_of(in), col_of(in));
        }
    }
};

template <> struct action<grammar::float_literal> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        const double v = std::stod(in.string());
        s.push(make_expr(LiteralNode{ Value{v} }, line_of(in), col_of(in)));
    }
};

template <> struct action<grammar::string_literal> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        s.push(make_expr(
            LiteralNode{ Value{decode_string_literal(in.string_view())} },
            line_of(in), col_of(in)));
    }
};

template <> struct action<grammar::kw_true> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        s.push(make_expr(LiteralNode{ Value{true} }, line_of(in), col_of(in)));
    }
};
template <> struct action<grammar::kw_false> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        s.push(make_expr(LiteralNode{ Value{false} }, line_of(in), col_of(in)));
    }
};

// -- field access --

template <> struct action<grammar::field_dot> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        // The full match is "tags.NAME". Strip the leading "tags." (we don't
        // need to keep the prefix; field name is what matters).
        std::string text{in.string()};
        const auto pos = text.find('.');
        std::string name = (pos == std::string::npos) ? text : text.substr(pos + 1);
        // Strip whitespace introduced by `ws` rules around the dot.
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
        while (!name.empty() && (name.back()  == ' ' || name.back()  == '\t')) name.pop_back();
        s.push(make_expr(FieldNode{std::move(name)}, line_of(in), col_of(in)));
    }
};

template <> struct action<grammar::field_index> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        // The string_literal action already pushed a Value{string} Literal.
        // Convert that into a FieldNode using its string content.
        auto top = s.pop();
        if (!top->is<LiteralNode>() || !top->as<LiteralNode>().value.is_string()) {
            throw ParseError("tags[\"...\"] expects a string literal",
                             line_of(in), col_of(in));
        }
        const std::string name = top->as<LiteralNode>().value.as_string();
        s.push(make_expr(FieldNode{name}, line_of(in), col_of(in)));
    }
};

// -- comparison operator tags --
// These don't push expressions; they record the operator for the upcoming
// rhs to use.

template <> struct action<grammar::op_eq>  { static void apply0(State& s){ s.pending_comp = BinOp::Eq;    s.pending_is_in=false; } };
template <> struct action<grammar::op_neq> { static void apply0(State& s){ s.pending_comp = BinOp::NotEq; s.pending_is_in=false; } };
template <> struct action<grammar::op_lt>  { static void apply0(State& s){ s.pending_comp = BinOp::Lt;    s.pending_is_in=false; } };
template <> struct action<grammar::op_gt>  { static void apply0(State& s){ s.pending_comp = BinOp::Gt;    s.pending_is_in=false; } };
template <> struct action<grammar::op_lte> { static void apply0(State& s){ s.pending_comp = BinOp::LtEq;  s.pending_is_in=false; } };
template <> struct action<grammar::op_gte> { static void apply0(State& s){ s.pending_comp = BinOp::GtEq;  s.pending_is_in=false; } };
template <> struct action<grammar::kw_in>  { static void apply0(State& s){ s.pending_is_in = true; } };

template <> struct action<grammar::comp_tail> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        auto rhs = s.pop();
        auto lhs = s.pop();
        const BinOp op = s.pending_is_in ? BinOp::In : s.pending_comp;
        s.push(make_expr(
            BinaryNode{op, std::move(lhs), std::move(rhs)},
            line_of(in), col_of(in)));
    }
};

// -- boolean operators --

template <> struct action<grammar::not_tail> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        auto operand = s.pop();
        s.push(make_expr(
            UnaryNode{UnOp::LogicalNot, std::move(operand)},
            line_of(in), col_of(in)));
    }
};

template <> struct action<grammar::and_tail> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        auto rhs = s.pop();
        auto lhs = s.pop();
        s.push(make_expr(
            BinaryNode{BinOp::LogicalAnd, std::move(lhs), std::move(rhs)},
            line_of(in), col_of(in)));
    }
};

template <> struct action<grammar::or_tail> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        auto rhs = s.pop();
        auto lhs = s.pop();
        s.push(make_expr(
            BinaryNode{BinOp::LogicalOr, std::move(lhs), std::move(rhs)},
            line_of(in), col_of(in)));
    }
};

// -- unary minus --

template <> struct action<grammar::neg_unary> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        auto operand = s.pop();
        s.push(make_expr(
            UnaryNode{UnOp::Negate, std::move(operand)},
            line_of(in), col_of(in)));
    }
};

// -- function call: ident(args) --
//
// At the time `call` fires, the args (if any) are on the stack in left-to-right
// order. We need the function name (captured from the leading ident). PEGTL
// re-matches the rule's full text via `in.string()`, so we extract the ident.

template <> struct action<grammar::call> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        const std::string text = in.string();
        // Find the '(' that ends the function name.
        auto paren_pos = text.find('(');
        std::string name = text.substr(0, paren_pos);
        // Trim trailing whitespace.
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) {
            name.pop_back();
        }
        // Args were pushed by `expr` actions during the inner parse. The
        // number pushed equals the number of args. We tracked nothing
        // explicit, so use a stack-marker approach: count items above the
        // saved marker. To keep things simple, walk down the stack from the
        // top collecting until we see... actually the cleanest path is to
        // know the arg count from the grammar. PEGTL doesn't expose that
        // here, so we use a different trick: stash a marker before args
        // start. We do that by emitting an action on the opening paren.
        //
        // For simplicity in this first cut, we count args based on commas in
        // the captured text. This is correct since args are non-recursive at
        // this level — nested calls have already collapsed to single Expr
        // values on the stack before we get here.
        //
        // Find the matching closing paren and count top-level commas.
        std::size_t depth = 0;
        std::size_t args = 0;
        bool saw_non_ws = false;
        for (std::size_t i = paren_pos; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') { depth++; continue; }
            if (c == ')') { depth--; if (depth == 0) break; continue; }
            if (depth == 1) {
                if (c == ',') { args++; }
                else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    saw_non_ws = true;
                }
            }
        }
        if (saw_non_ws) ++args;  // commas + 1, or 1 if no commas and non-empty body

        // Pop args (they're in left-to-right order, so we need to reverse).
        std::vector<ExprPtr> arg_exprs;
        arg_exprs.reserve(args);
        for (std::size_t i = 0; i < args; ++i) arg_exprs.push_back(s.pop());
        std::reverse(arg_exprs.begin(), arg_exprs.end());

        s.push(make_expr(
            CallNode{std::move(name), std::move(arg_exprs)},
            line_of(in), col_of(in)));
    }
};

// -- method call: .ident(args) --
//
// The receiver is on the stack (pushed by primary/postfix recursion). We pop
// it as the call's target.

template <> struct action<grammar::method_call> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        const std::string text = in.string();
        auto dot_pos = text.find('.');
        auto paren_pos = text.find('(', dot_pos);
        std::string method = text.substr(dot_pos + 1, paren_pos - dot_pos - 1);
        while (!method.empty() && (method.back() == ' ' || method.back() == '\t')) {
            method.pop_back();
        }

        // Count args same as above.
        std::size_t depth = 0;
        std::size_t args = 0;
        bool saw_non_ws = false;
        for (std::size_t i = paren_pos; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') { depth++; continue; }
            if (c == ')') { depth--; if (depth == 0) break; continue; }
            if (depth == 1) {
                if (c == ',') { args++; }
                else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                    saw_non_ws = true;
                }
            }
        }
        if (saw_non_ws) ++args;

        std::vector<ExprPtr> arg_exprs;
        arg_exprs.reserve(args);
        for (std::size_t i = 0; i < args; ++i) arg_exprs.push_back(s.pop());
        std::reverse(arg_exprs.begin(), arg_exprs.end());

        auto receiver = s.pop();
        s.push(make_expr(
            MethodCallNode{std::move(receiver), std::move(method), std::move(arg_exprs)},
            line_of(in), col_of(in)));
    }
};

// -- tuple or group --
//
// Strategy: at the opening paren we record the current stack size; at the
// closing paren we look at how many items were pushed by the body. We also
// scan the matched text for top-level commas to distinguish `(x)` (a single
// grouped expression — no-op) from `(x,)` (a 1-tuple — wrap in ListNode).
//
// Counting "top level" means we have to track paren depth: inside this
// rule's match the very first char is `(` which puts us at depth 1.
// Commas at depth == 1 are this group's commas; deeper commas belong to
// nested calls/tuples whose actions already consumed them.

template <> struct action<grammar::group_open> {
    static void apply0(State& s) {
        s.group_stack.push_back({s.stack.size()});
    }
};

template <> struct action<grammar::tuple_or_group> {
    template <class Input>
    static void apply(const Input& in, State& s) {
        if (s.group_stack.empty()) {
            throw InternalError("DSL parser: group_stack underflow");
        }
        const auto frame = s.group_stack.back();
        s.group_stack.pop_back();

        const std::size_t items_pushed = s.stack.size() - frame.stack_size_at_open;

        // Count top-level commas in the matched text.
        const std::string text = in.string();
        std::size_t depth = 0;
        std::size_t commas = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(') { ++depth; continue; }
            if (c == ')') {
                --depth;
                if (depth == 0) break;
                continue;
            }
            if (depth == 1 && c == ',') ++commas;
        }

        if (items_pushed == 0) {
            // Empty tuple: () — push an empty list.
            s.push(make_expr(ListNode{{}}, line_of(in), col_of(in)));
            return;
        }
        if (items_pushed == 1 && commas == 0) {
            // Plain grouping: (expr). The inner action already pushed; no-op.
            return;
        }

        // Tuple: 1+ items with at least one comma, OR 2+ items. Bundle the
        // top `items_pushed` stack entries into a ListNode.
        std::vector<ExprPtr> items;
        items.reserve(items_pushed);
        for (std::size_t i = 0; i < items_pushed; ++i) items.push_back(s.pop());
        std::reverse(items.begin(), items.end());
        s.push(make_expr(ListNode{std::move(items)}, line_of(in), col_of(in)));
    }
};

// ---- parse() ------------------------------------------------------------

// Hard limits enforced before handing source to PEGTL. The grammar is
// recursive-descent and trivially stack-overflowable on deeply nested
// expressions — a stack overflow is undefined behavior in C++, not an
// exception, and the SIGSEGV would kill the router process. The
// Python-side preflight in api/models.py applies the same limits for
// the API path; this guards the routes that bypass the API (rule
// inserts via psql, future migrations) and the cache-refresh path
// re-parsing existing rules on every cycle.
constexpr std::size_t kMaxSourceLength = 8 * 1024;   // bytes
constexpr int         kMaxParenDepth   = 32;         // matches API preflight

namespace {
void check_source_limits(std::string_view source) {
    if (source.size() > kMaxSourceLength) {
        throw ParseError(
            "predicate exceeds maximum length (" +
                std::to_string(source.size()) + " > " +
                std::to_string(kMaxSourceLength) + " bytes)",
            1, 1);
    }
    // Linear scan: track paren depth, skip past string-literal contents.
    int depth = 0, max_depth = 0;
    char in_str = 0;
    for (char c : source) {
        if (in_str) {
            if (c == in_str) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'') in_str = c;
        else if (c == '(') { ++depth; if (depth > max_depth) max_depth = depth; }
        else if (c == ')') --depth;
    }
    if (max_depth > kMaxParenDepth) {
        throw ParseError(
            "predicate nesting depth " + std::to_string(max_depth) +
                " exceeds limit " + std::to_string(kMaxParenDepth),
            1, 1);
    }
}
}  // namespace

ExprPtr parse(std::string_view source) {
    check_source_limits(source);
    pegtl::memory_input in(source.data(), source.size(), "rule");
    State state;
    try {
        const bool ok = pegtl::parse<grammar::rule, action>(in, state);
        if (!ok) {
            throw ParseError("syntax error",
                             in.position().line, in.position().column);
        }
    } catch (const pegtl::parse_error& e) {
        // PEGTL's parse_error carries position(s) — take the first.
        if (!e.positions().empty()) {
            const auto& p = e.positions().front();
            throw ParseError(std::string{e.message()}, p.line, p.column);
        }
        throw ParseError(std::string{e.what()}, 1, 1);
    }
    if (state.stack.size() != 1) {
        throw InternalError("DSL parser: stack ended at size " +
                            std::to_string(state.stack.size()) +
                            " (expected 1)");
    }
    return std::move(state.stack.front());
}

}  // namespace nl_router::dsl
