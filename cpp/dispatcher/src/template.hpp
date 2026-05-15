// nl-dispatch/template.hpp
//
// Tiny `${TagName}` template engine for destination configs (path
// templates, URL templates, body templates). The substitution alphabet
// is intentionally narrow:
//
//   ${Identifier}    — replaced by tags[Identifier] from the work_queue
//                       tags JSONB, or "" if absent.
//
// No nested expressions, no method chains, no conditionals — anything
// more elaborate belongs in the rule DSL, not in destination configs.

#pragma once

#include <string>
#include <string_view>

namespace nlr {

// Expand `${Name}` references in `template_str` using values from a
// JSONB-formatted `tags_json` object. Missing keys substitute as empty
// strings (matches dicomdiablo's "missing tag = '' " behavior).
//
// Unrecognized escapes (`$something_not_a_brace`, `$${escaped}`) pass
// through verbatim; literal `$` followed by `{` is the only special form.
std::string expand_template(std::string_view template_str,
                             std::string_view tags_json);

// URL-encode a string (RFC 3986 unreserved + percent-encode everything else).
// Used by handlers that splice tag values into URL paths.
std::string url_encode(std::string_view s);

}  // namespace nlr
