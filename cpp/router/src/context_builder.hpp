// nl-route/context_builder.hpp
//
// Builds a `dsl::Context` (the name → value lookup the rule evaluator uses)
// from one work_queue row's data.
//
// What goes into the Context:
//
//   1. Every key in the work_queue.tags JSONB column, with its PascalCase
//      DICOM name (Modality, SeriesDescription, PatientID, ...) and string
//      value. This is what dicomdiablo-compatible rules reference via
//      `tags.<Name>`.
//
//   2. The network-derived names that aren't in DICOM tags but rules need
//      to predicate on:
//        SenderAET   <- work_queue.calling_aet
//        ReceiverAET <- work_queue.called_aet
//        PeerIP      <- work_queue.peer_ip
//
// In M1 we use string-value semantics throughout — DICOM tag values arrive
// as strings, and the DSL's `float()` / `int()` builtins handle numeric
// coercion when a rule needs comparison against a number.

#pragma once

#include <string>
#include <string_view>

#include "nl_router/dsl/context.hpp"

namespace nlr {

// All the per-row fields the context builder needs. Mirrors the columns
// the router fetches from work_queue. Strings are values-as-stored;
// `tags_json` is the raw JSONB text returned by Postgres for the tags
// column (we parse it inside the builder).
struct RowContextInput {
    std::string calling_aet;
    std::string called_aet;
    std::string peer_ip;
    std::string tags_json;   // serialized JSONB; '{...}' object expected
};

// Build a dsl::Context for evaluating rule predicates against one
// work_queue row.
//
// Throws std::runtime_error if tags_json cannot be parsed as a JSON object.
::nl_router::dsl::Context build_context(const RowContextInput& input);

}  // namespace nlr
