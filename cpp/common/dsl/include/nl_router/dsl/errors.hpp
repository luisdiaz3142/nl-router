// nl_router/dsl/errors.hpp
//
// Exceptions thrown by the rule DSL parser and evaluator.
//
// We use stdlib exceptions (std::runtime_error) so callers can catch them
// without depending on our types. Most rule-evaluation errors will be caught
// at the Router boundary and logged with the offending rule_id and
// work_queue_id; very few should propagate further.
#pragma once

#include <stdexcept>
#include <string>

namespace nl_router::dsl {

// Base class for any DSL-related error so callers can do
// `catch (const dsl::Error&)` regardless of subtype.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thrown by parse() when the input is syntactically invalid.
// Carries 1-based line/column of the failure point.
class ParseError : public Error {
public:
    ParseError(std::string msg, std::size_t line, std::size_t column)
        : Error{format(msg, line, column)},
          line_{line},
          column_{column},
          raw_message_{std::move(msg)} {}

    std::size_t line()    const noexcept { return line_; }
    std::size_t column()  const noexcept { return column_; }
    const std::string& raw_message() const noexcept { return raw_message_; }

private:
    static std::string format(const std::string& msg, std::size_t l, std::size_t c) {
        return msg + " (at line " + std::to_string(l) +
               ", column " + std::to_string(c) + ")";
    }
    std::size_t line_;
    std::size_t column_;
    std::string raw_message_;
};

// Thrown at evaluation time when a value cannot participate in the requested
// operation (e.g. comparing a list to a number, applying .lower() to an int).
class TypeError : public Error {
public:
    using Error::Error;
};

// Thrown at evaluation time when a referenced function is not part of the
// allowed builtin set. The grammar prevents undefined identifiers from
// parsing, so this fires only for explicit "future" builtins that aren't
// implemented yet.
class NotImplementedError : public Error {
public:
    using Error::Error;
};

// Thrown when the evaluator hits a value/state combination that should be
// unreachable given the grammar — bug, not user error.
class InternalError : public Error {
public:
    using Error::Error;
};

}  // namespace nl_router::dsl
