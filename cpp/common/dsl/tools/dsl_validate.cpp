// nl-dsl-validate — parse a predicate from stdin, exit 0 on success.
//
// Shell-out target for the management API's rule create/update path.
// Up to M22 the API only structurally validated the predicate (length +
// paren balance) and the actual DSL parse happened later inside the
// router's rule_cache_refresh loop. A syntactically invalid rule would
// save successfully and only fail silently in the router log every 15
// seconds — operators didn't learn about the bug until they tried to
// send a study and nothing routed.
//
// This binary closes that gap by exposing the exact same parse pipeline
// the router uses at HTTP-POST time. The Python API shells out, hands
// the predicate over stdin, and checks the return code. ~1 ms call,
// no shared library bridge to maintain.
//
// Contract:
//   stdin   : predicate text (entire stream until EOF)
//   stdout  : nothing on success
//   stderr  : single-line error message on failure
//   exit 0  : predicate parsed cleanly
//   exit 1  : ParseError; stderr has the message
//   exit 2  : I/O or internal error; stderr has the message

#include <iostream>
#include <sstream>
#include <string>

#include "nl_router/dsl/dsl.hpp"
#include "nl_router/dsl/errors.hpp"

int main() {
    try {
        // Slurp stdin. Predicates are tiny (max 8 KiB enforced by the
        // API model), so reading the whole stream is fine.
        std::ostringstream buf;
        buf << std::cin.rdbuf();
        const std::string source = buf.str();

        if (source.empty()) {
            std::cerr << "empty predicate\n";
            return 1;
        }

        // parse() throws ParseError on syntactic failure with line/column
        // baked into the message; the API surfaces that verbatim to the
        // operator.
        (void)nl_router::dsl::parse(source);
        return 0;
    } catch (const nl_router::dsl::ParseError& e) {
        std::cerr << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "internal: " << e.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "internal: unknown\n";
        return 2;
    }
}
