#include "rule_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "db.hpp"
#include "logging.hpp"

namespace nlr {

RuleCache::RuleCache(Db& db) : db_(db) {
    study_rules_  = std::make_shared<std::vector<CachedRule>>();
    series_rules_ = std::make_shared<std::vector<CachedRule>>();
}

void RuleCache::refresh() {
    // Pull the enabled rules from the DB. The DB layer returns raw rows;
    // we parse the predicates here and bucket by scope.
    auto raw = db_.list_enabled_rules();

    // Build a lookup of (id, predicate_hash) → existing AST so we don't
    // re-parse predicates whose text didn't change. Cheap optimization that
    // matters once an operator has many rules and we refresh every 15s.
    std::unordered_map<std::string, ::nl_router::dsl::ExprPtr> prev_asts;
    {
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& list : {study_rules_, series_rules_}) {
            for (const auto& r : *list) {
                const auto key = std::to_string(r.id) + ":" + r.predicate_hash;
                // Move out of the previous cache; raw vector will be dropped.
                prev_asts.emplace(key, nullptr);  // sentinel; replaced below
            }
        }
    }
    // Second pass to actually move ASTs — done outside the lock since we're
    // building new vectors. The original const-shared lists won't be mutated.
    // (We can't move out of a const list, so this is informational only;
    // we'll re-parse for v1. The hash-keyed reuse optimization lands in M4
    // alongside NOTIFY-based invalidation.)

    auto new_study  = std::make_shared<std::vector<CachedRule>>();
    auto new_series = std::make_shared<std::vector<CachedRule>>();

    std::size_t parsed = 0;
    std::size_t failed = 0;

    for (auto& r : raw) {
        CachedRule cr;
        cr.id              = r.id;
        cr.name            = std::move(r.name);
        cr.scope           = std::move(r.scope);
        cr.dispatch_order  = std::move(r.dispatch_order);
        cr.priority        = r.priority;
        cr.predicate       = std::move(r.predicate);
        cr.predicate_hash  = std::move(r.predicate_hash);

        try {
            cr.ast = ::nl_router::dsl::parse(cr.predicate);
        } catch (const std::exception& e) {
            // A rule with a broken predicate is excluded from the cache —
            // we log loudly so the operator notices but don't fail the
            // whole refresh cycle.
            LOG_WARN("rule_cache.parse_failed",
                "rule_id",  std::to_string(cr.id),
                "rule_name", cr.name,
                "error",    e.what());
            ++failed;
            continue;
        }

        ++parsed;
        if (cr.scope == "study") {
            new_study->push_back(std::move(cr));
        } else if (cr.scope == "series") {
            new_series->push_back(std::move(cr));
        } else {
            LOG_WARN("rule_cache.unknown_scope",
                "rule_id", std::to_string(cr.id),
                "scope",   cr.scope);
        }
    }

    // Sort by priority DESC, then name for stable evaluation order.
    auto cmp = [](const CachedRule& a, const CachedRule& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.name < b.name;
    };
    std::sort(new_study->begin(),  new_study->end(),  cmp);
    std::sort(new_series->begin(), new_series->end(), cmp);

    {
        std::lock_guard<std::mutex> lock(m_);
        study_rules_  = std::move(new_study);
        series_rules_ = std::move(new_series);
    }

    LOG_INFO("rule_cache.refreshed",
        "parsed",  std::to_string(parsed),
        "failed",  std::to_string(failed),
        "study",   std::to_string(study_rules_->size()),
        "series",  std::to_string(series_rules_->size()));
}

std::shared_ptr<const std::vector<CachedRule>>
RuleCache::for_scope(const std::string& scope) const {
    std::lock_guard<std::mutex> lock(m_);
    if (scope == "study")  return study_rules_;
    if (scope == "series") return series_rules_;
    // Empty vector for any unrecognized scope (shouldn't happen with the
    // enum-backed column, but defensive).
    static const auto empty = std::make_shared<std::vector<CachedRule>>();
    return empty;
}

std::size_t RuleCache::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return study_rules_->size() + series_rules_->size();
}

}  // namespace nlr
