// PEGTL grammar for the rule expression DSL.
//
// Grammar (informal):
//
//     expr        ::= or_expr
//     or_expr     ::= and_expr ("or" and_expr)*
//     and_expr    ::= not_expr ("and" not_expr)*
//     not_expr    ::= "not" not_expr | comp_expr
//     comp_expr   ::= add_expr (comp_op add_expr | "in" add_expr)?
//     comp_op     ::= "==" | "!=" | "<=" | ">=" | "<" | ">"
//     add_expr    ::= unary
//     unary       ::= "-" unary | postfix
//     postfix     ::= primary ( "." ident "(" args? ")" )*
//     primary     ::= literal | tuple | field | call | "(" expr ")"
//     literal     ::= number | string | bool
//     number      ::= int | float
//     string      ::= "..." | '...'
//     bool        ::= "True" | "False"
//     tuple       ::= "(" expr ("," expr)* ","? ")"     # parens with 2+ or trailing comma
//                   | "(" ")"                            # empty tuple
//     field       ::= "tags" "." ident
//                   | "tags" "[" string "]"
//     call        ::= ident "(" args? ")"
//     args        ::= expr ("," expr)*
//     ident       ::= /[A-Za-z_][A-Za-z_0-9]*/
//
// Notes
// -----
// * Operator precedence (high → low): postfix > unary > comp > not > and > or.
// * The `in` operator is a comparison (same precedence as `==` etc.); it is
//   NOT chainable — `a in b in c` is a parse error.
// * Method calls bind tighter than unary minus, matching Python.
// * The grammar is intentionally small so the sandbox boundary is clear.

#pragma once

#include <tao/pegtl.hpp>

namespace nl_router::dsl::grammar {

namespace pegtl = tao::pegtl;

// ---- Tokens -------------------------------------------------------------

// Match "and"/"or"/"not"/"in"/"True"/"False" as full words (not prefixes of
// longer identifiers like "and_index"). PEGTL's seq + at + not_at trick.
struct kw_end : pegtl::not_at<pegtl::sor<pegtl::alnum, pegtl::one<'_'>>> {};

template <char... C>
struct keyword : pegtl::seq<pegtl::string<C...>, kw_end> {};

struct kw_and   : keyword<'a','n','d'> {};
struct kw_or    : keyword<'o','r'>     {};
struct kw_not   : keyword<'n','o','t'> {};
struct kw_in    : keyword<'i','n'>     {};
struct kw_true  : keyword<'T','r','u','e'> {};
struct kw_false : keyword<'F','a','l','s','e'> {};

// Identifiers (used for field names, method names, function names).
// Reserved keywords are excluded — handled by ordering in the grammar rather
// than negative lookahead, to keep the grammar readable.
struct ident_first : pegtl::sor<pegtl::alpha, pegtl::one<'_'>> {};
struct ident_rest  : pegtl::sor<pegtl::alnum, pegtl::one<'_'>> {};
struct ident       : pegtl::seq<ident_first, pegtl::star<ident_rest>> {};

// Whitespace. Newlines are allowed but not commonly used in inline rules.
struct ws : pegtl::star<pegtl::sor<pegtl::space, pegtl::eol>> {};

// ---- Literals -----------------------------------------------------------

// Integer literal: optional minus is handled by `unary`, not here.
struct int_literal : pegtl::plus<pegtl::digit> {};

// Float literal: digits "." digits, with digits required on at least one side.
// Order matters: try float before int in `number` so `1.5` doesn't parse as
// `1` followed by `.5`.
struct float_literal : pegtl::seq<
    pegtl::plus<pegtl::digit>,
    pegtl::one<'.'>,
    pegtl::star<pegtl::digit>
> {};

// String literals. Support both single and double quotes (Python-compatible).
// Escape sequences: \\, \", \', \n, \t, \r, \0.
struct escape_char : pegtl::one<'\\','\'','"','n','t','r','0'> {};
struct dq_char : pegtl::sor<
    pegtl::seq<pegtl::one<'\\'>, escape_char>,
    pegtl::not_one<'"','\\','\n'>
> {};
struct sq_char : pegtl::sor<
    pegtl::seq<pegtl::one<'\\'>, escape_char>,
    pegtl::not_one<'\'','\\','\n'>
> {};
struct double_quoted : pegtl::if_must<pegtl::one<'"'>,  pegtl::star<dq_char>, pegtl::one<'"'>> {};
struct single_quoted : pegtl::if_must<pegtl::one<'\''>, pegtl::star<sq_char>, pegtl::one<'\''>> {};
struct string_literal : pegtl::sor<double_quoted, single_quoted> {};

// Boolean literal.
struct bool_literal : pegtl::sor<kw_true, kw_false> {};

// ---- Field access -------------------------------------------------------

struct kw_tags : pegtl::seq<pegtl::string<'t','a','g','s'>, kw_end> {};

// `tags.X`
struct field_dot : pegtl::seq<
    kw_tags, ws, pegtl::one<'.'>, ws, ident
> {};

// `tags["X"]`
struct field_index : pegtl::seq<
    kw_tags, ws, pegtl::one<'['>, ws, string_literal, ws, pegtl::one<']'>
> {};

struct field : pegtl::sor<field_dot, field_index> {};

// ---- Function calls -----------------------------------------------------

struct expr;   // forward decl — defined below

struct args_list : pegtl::seq<
    expr, ws,
    pegtl::star<pegtl::seq<pegtl::one<','>, ws, expr, ws>>
> {};

// `ident(arg, arg, ...)` — call without preceding dot. Used for builtins like
// float(), int(), len(), etc. The grammar accepts ANY identifier; the
// evaluator decides what's a known builtin and errors otherwise.
struct call : pegtl::seq<
    ident, ws,
    pegtl::one<'('>, ws,
    pegtl::opt<args_list>,
    pegtl::one<')'>
> {};

// ---- Parenthesized / tuple ---------------------------------------------

// (a, b, c) → tuple
// (a) → just a (parens grouping)
// (a,) → 1-tuple
// () → empty tuple
//
// PEGTL doesn't backtrack across rules easily, so we collapse this into a
// single rule that uses a separator-tracking action to distinguish "grouping
// parens" from "tuple". Implementation detail handled in parser.cpp via
// counting commas seen.

// Open / close terminals are their own rules so the parser can hang actions
// on them (push a stack-marker on open; consume the marker on close to know
// exactly how many items the inner expression pushed).
struct group_open  : pegtl::one<'('> {};
struct group_close : pegtl::one<')'> {};

struct tuple_or_group : pegtl::seq<
    group_open, ws,
    pegtl::opt<pegtl::seq<expr, ws,
        pegtl::star<pegtl::seq<pegtl::one<','>, ws, pegtl::opt<expr>, ws>>
    >>,
    group_close
> {};

// ---- primary ------------------------------------------------------------

// Order matters: more-specific patterns first.
struct primary : pegtl::sor<
    field,
    bool_literal,
    float_literal,
    int_literal,
    string_literal,
    call,
    tuple_or_group
> {};

// ---- postfix (method calls .lower() / .startswith("X")) -----------------

struct method_call : pegtl::seq<
    pegtl::one<'.'>, ws,
    ident, ws,
    pegtl::one<'('>, ws,
    pegtl::opt<args_list>,
    pegtl::one<')'>
> {};

struct postfix : pegtl::seq<primary, pegtl::star<pegtl::seq<ws, method_call>>> {};

// ---- unary --------------------------------------------------------------

struct unary;
struct neg_unary : pegtl::seq<pegtl::one<'-'>, ws, unary> {};
struct unary : pegtl::sor<neg_unary, postfix> {};

// ---- comparison ---------------------------------------------------------

// Two-character operators must be tried before their one-character prefixes.
// PEGTL's sor is leftmost-match, so order matters.
struct op_eq   : pegtl::string<'=','='> {};
struct op_neq  : pegtl::string<'!','='> {};
struct op_lte  : pegtl::string<'<','='> {};
struct op_gte  : pegtl::string<'>','='> {};
struct op_lt   : pegtl::one<'<'> {};
struct op_gt   : pegtl::one<'>'> {};

struct comp_op : pegtl::sor<op_eq, op_neq, op_lte, op_gte, op_lt, op_gt> {};

struct comp_tail : pegtl::sor<
    pegtl::seq<comp_op, ws, unary>,
    pegtl::seq<kw_in,   ws, unary>
> {};

struct comp_expr : pegtl::seq<unary, pegtl::opt<pegtl::seq<ws, comp_tail>>> {};

// ---- not / and / or -----------------------------------------------------

struct not_expr;
struct not_tail : pegtl::seq<kw_not, ws, not_expr> {};
struct not_expr : pegtl::sor<not_tail, comp_expr> {};

struct and_tail : pegtl::seq<ws, kw_and, ws, not_expr> {};
struct and_expr : pegtl::seq<not_expr, pegtl::star<and_tail>> {};

struct or_tail : pegtl::seq<ws, kw_or, ws, and_expr> {};
struct or_expr : pegtl::seq<and_expr, pegtl::star<or_tail>> {};

struct expr : or_expr {};

// ---- Top-level entry ----------------------------------------------------

struct rule : pegtl::seq<ws, expr, ws, pegtl::eof> {};

}  // namespace nl_router::dsl::grammar
