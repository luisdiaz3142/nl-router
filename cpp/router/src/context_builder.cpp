#include "context_builder.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace nlr {

::nl_router::dsl::Context build_context(const RowContextInput& input) {
    ::nl_router::dsl::Context ctx;

    // 1) JSONB tags. The receiver writes string values only (numeric DICOM
    //    types like DS/IS are kept as their original string form to preserve
    //    precision). We walk the top-level object and push each key/value
    //    pair into the context.
    //
    //    Non-string values are stringified (e.g. accidental numeric tags
    //    from a non-receiver row) so the DSL's `tags.Foo == "x"` predicates
    //    behave consistently.
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(input.tags_json);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string{"failed to parse tags JSONB: "} + e.what());
    }
    if (!parsed.is_object()) {
        throw std::runtime_error("tags JSONB is not a JSON object");
    }

    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        const auto& key = it.key();
        const auto& val = it.value();
        std::string s;
        if (val.is_string()) {
            s = val.get<std::string>();
        } else if (val.is_null()) {
            continue;  // skip nulls — same as "missing" in the DSL
        } else {
            // Stringify booleans / numbers / etc. so the DSL sees a value
            // it can do == "x" / float() / int() against.
            s = val.dump();
        }
        ctx.set(key, std::move(s));
    }

    // 2) Network-derived names that aren't in the DICOM dataset. dicomdiablo
    //    rules access these as tags.SenderAET / tags.ReceiverAET — match the
    //    same names.
    ctx.set("SenderAET",   input.calling_aet);
    ctx.set("ReceiverAET", input.called_aet);
    ctx.set("PeerIP",      input.peer_ip);

    // Also accept the canonical snake_case column names so a rule author who
    // prefers the database-side spelling works too.
    ctx.set_alias("calling_aet", "SenderAET");
    ctx.set_alias("called_aet",  "ReceiverAET");
    ctx.set_alias("peer_ip",     "PeerIP");

    return ctx;
}

}  // namespace nlr
