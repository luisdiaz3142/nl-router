// Implementation of nl_router::dsl::Context.

#include "nl_router/dsl/context.hpp"

namespace nl_router::dsl {

void Context::set(std::string name, std::string value) {
    fields_[std::move(name)] = std::move(value);
}

void Context::set_alias(std::string alias, std::string target) {
    aliases_[std::move(alias)] = std::move(target);
}

std::optional<std::string> Context::get(std::string_view name) const {
    // Try direct lookup first (most common path).
    if (auto it = fields_.find(std::string{name}); it != fields_.end()) {
        return it->second;
    }
    // Try alias resolution. Aliases are one-deep; we don't chain.
    if (auto it = aliases_.find(std::string{name}); it != aliases_.end()) {
        if (auto resolved = fields_.find(it->second); resolved != fields_.end()) {
            return resolved->second;
        }
    }
    return std::nullopt;
}

void Context::clear() noexcept {
    fields_.clear();
    aliases_.clear();
}

}  // namespace nl_router::dsl
