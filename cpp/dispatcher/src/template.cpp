#include "template.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nlr {

namespace {

// Parse the work_queue tags JSONB once. Returns a flat name→string map.
// Non-string values get stringified so the operator-facing template
// behavior is consistent regardless of the underlying VR.
std::unordered_map<std::string, std::string>
parse_tags(std::string_view tags_json) {
    std::unordered_map<std::string, std::string> out;
    if (tags_json.empty()) return out;
    try {
        const auto j = nlohmann::json::parse(tags_json);
        if (!j.is_object()) return out;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                out[it.key()] = it.value().get<std::string>();
            } else if (!it.value().is_null()) {
                out[it.key()] = it.value().dump();
            }
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed JSONB → empty map; handler then sees missing tags
        // as empty strings and operator notices via the dispatch result.
    }
    return out;
}

}  // namespace

std::string expand_template(std::string_view tmpl, std::string_view tags_json) {
    const auto tags = parse_tags(tags_json);

    std::string out;
    out.reserve(tmpl.size());

    for (std::size_t i = 0; i < tmpl.size(); ) {
        const char c = tmpl[i];
        // Look for `${...}`. Anything else is a literal pass-through.
        if (c != '$' || i + 1 >= tmpl.size() || tmpl[i + 1] != '{') {
            out.push_back(c);
            ++i;
            continue;
        }
        // Find the closing `}`. If missing, treat the whole `${...` as
        // literal — better than throwing on a malformed template.
        const auto close = tmpl.find('}', i + 2);
        if (close == std::string_view::npos) {
            out.append(tmpl.substr(i));
            break;
        }
        const auto name = std::string{tmpl.substr(i + 2, close - i - 2)};
        if (auto it = tags.find(name); it != tags.end()) {
            out.append(it->second);
        }
        // else: missing tag → empty string (consistent w/ DSL behavior).
        i = close + 1;
    }
    return out;
}

std::string url_encode(std::string_view s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0xF]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

}  // namespace nlr
