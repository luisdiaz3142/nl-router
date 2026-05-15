// nl_router/dsl/context.hpp
//
// Context is the field-lookup table the evaluator queries when a rule
// expression references `tags.X`.
//
// Field resolution order (per the design plan) is the Router's responsibility
// to feed into the Context — typically:
//   1. Built-in alias  (e.g. "SenderAET"  -> work_queue.calling_aet)
//   2. Snake-case scalar column         (e.g. "calling_aet")
//   3. JSONB fallback                  (work_queue.tags->>'FieldName')
//
// All three sources are normalized to strings (DICOM tag values are strings;
// numeric casts happen at evaluation time via float()/int()). The Context
// just answers "is this field present?" and "what is its string value?".
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nl_router::dsl {

class Context {
public:
    // Set a field value. Overwrites any prior value for the same name.
    void set(std::string name, std::string value);

    // Set an alias so two names resolve to the same value. Aliases are useful
    // for the dicomdiablo-style names like "SenderAET" mapping to our
    // canonical "calling_aet". Resolution is one-deep — chains aren't
    // followed.
    void set_alias(std::string alias, std::string target);

    // Look up a field. Returns nullopt when the field is not present;
    // returns the string value otherwise. Aliases are resolved transparently.
    std::optional<std::string> get(std::string_view name) const;

    // Total number of explicitly-set fields (not counting aliases).
    std::size_t size() const noexcept { return fields_.size(); }

    // Drop all fields and aliases.
    void clear() noexcept;

private:
    std::unordered_map<std::string, std::string> fields_;
    std::unordered_map<std::string, std::string> aliases_;
};

}  // namespace nl_router::dsl
