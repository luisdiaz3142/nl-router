// nl-route/rule_cache.hpp
//
// In-memory cache of enabled rules with parsed AST. Loaded from the rules
// table at startup and refreshed periodically (config: rule_cache_refresh_s).
//
// We cache the parsed AST keyed by (rule_id, predicate_hash). When a rule's
// predicate text changes, the hash differs and we re-parse on next refresh.
// Disabled or removed rules are dropped from the cache automatically.
//
// Future: NOTIFY-based invalidation so operators see their edits applied
// without waiting for the refresh interval. Deferred until M4 wires up rule
// CRUD endpoints.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nl_router/dsl/dsl.hpp"

namespace nlr {

class Db;   // forward declaration

// One cached rule, ready to evaluate.
struct CachedRule {
    std::int64_t            id;
    std::string             name;
    std::string             scope;          // "study" | "series"
    std::string             dispatch_order; // "parallel" | "sequential"
    std::int16_t            priority;
    std::string             predicate;      // source text, for logs
    ::nl_router::dsl::ExprPtr ast;          // parsed predicate
    // predicate_hash kept around to detect changes without re-parsing.
    std::string             predicate_hash;
};

class RuleCache {
public:
    explicit RuleCache(Db& db);

    // Refresh the cache from the database. Re-uses ASTs for rules whose
    // predicate text hasn't changed (matched by predicate_hash).
    //
    // Thread-safe: callers may call `for_scope()` concurrently with
    // refresh() — refresh() swaps the rule list under a mutex.
    void refresh();

    // Return the enabled rules for the given scope, ordered by priority DESC
    // then name (matches the seed loaded from the DB).
    //
    // Returns a shared snapshot — callers can iterate without holding a
    // lock; a concurrent refresh() builds a new vector and atomically
    // swaps the snapshot pointer.
    std::shared_ptr<const std::vector<CachedRule>> for_scope(const std::string& scope) const;

    // Total count of cached rules (any scope). Used for logging.
    std::size_t size() const;

private:
    Db&                                                    db_;
    mutable std::mutex                                     m_;
    // Sharded by scope so each evaluate iteration grabs only the rules
    // that match the row's close_trigger / scope without scanning.
    std::shared_ptr<const std::vector<CachedRule>>         study_rules_;
    std::shared_ptr<const std::vector<CachedRule>>         series_rules_;
};

}  // namespace nlr
