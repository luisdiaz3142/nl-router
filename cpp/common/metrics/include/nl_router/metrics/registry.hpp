// nl-router/metrics/registry.hpp
//
// Minimal Prometheus-compatible metrics registry. Lives in cpp/common/ so all
// services (receiver, router, dispatcher, cleaner, module workers) can link
// the same primitives.
//
// Design constraints:
//   * Zero external deps. We render text-exposition format directly and
//     ship a tiny HTTP exposer (see exposer.hpp).
//   * Lock-free hot path (atomic increments). Lookups across distinct label
//     sets take a shared lock on the family map; the hot path caches the
//     metric reference and skips the lookup.
//   * No client-side cardinality protection — operators are expected to use
//     bounded label values (calling_aet, modality, etc.).
//
// Usage:
//   auto& reg = nlr::metrics::Registry::global();
//   auto& assoc_total = reg.counter("nl_receiver_associations_total",
//       "Total inbound associations by result", {"result"});
//   assoc_total.labels({"accepted"}).inc();
//
//   auto& dur = reg.histogram("nl_receiver_association_duration_seconds",
//       "Association duration",
//       {0.1, 0.5, 1, 2, 5, 10, 30, 60, 120, 300});
//   dur.observe(elapsed_seconds);
//
// Concurrency:
//   Counter::inc/get, Gauge::set/inc/dec/get, Histogram::observe are
//   thread-safe via std::atomic. Family-level metric registration uses
//   a shared_mutex; reads are the common path (already-registered labels)
//   and are uncontended after the first hit because labels() caches the
//   resolved metric pointer per call.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace nlr::metrics {

// ---- Primitive metric types --------------------------------------------

// Monotonically increasing counter. Reset only on process restart.
class Counter {
public:
    void inc(std::int64_t delta = 1) noexcept {
        if (delta > 0) value_.fetch_add(delta, std::memory_order_relaxed);
    }
    std::int64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
private:
    std::atomic<std::int64_t> value_ {0};
};

// Up/down value. Use for "currently active" measurements.
class Gauge {
public:
    void set(std::int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void inc(std::int64_t delta = 1) noexcept {
        value_.fetch_add(delta, std::memory_order_relaxed);
    }
    void dec(std::int64_t delta = 1) noexcept {
        value_.fetch_sub(delta, std::memory_order_relaxed);
    }
    std::int64_t get() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }
private:
    std::atomic<std::int64_t> value_ {0};
};

// Cumulative histogram. Buckets are cumulative ("le" — less-than-or-equal)
// per Prometheus convention. Sum is the running sum of all observations;
// count is the total number of observations.
class Histogram {
public:
    explicit Histogram(std::vector<double> bucket_bounds);

    void observe(double v) noexcept;

    // For exposition / tests.
    const std::vector<double>& bounds() const noexcept { return bounds_; }
    std::vector<std::int64_t> bucket_counts() const;   // cumulative
    double sum() const noexcept;
    std::int64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::vector<double>                       bounds_;        // upper bounds, sorted
    std::vector<std::unique_ptr<std::atomic<std::int64_t>>> buckets_; // per-bound count
    std::atomic<std::int64_t>                 count_ {0};     // total observations
    // double atomics aren't portably lock-free; use a CAS loop on uint64 bits.
    std::atomic<std::uint64_t>                sum_bits_ {0};
};

// ---- Label set ---------------------------------------------------------

// A label set is an ordered tuple of values keyed by the family's label
// names. We use a vector<string> in label-name order; that gives stable
// equality + hash without per-call map traversal.
struct LabelValues {
    std::vector<std::string> values;
    bool operator==(const LabelValues& o) const { return values == o.values; }
};
struct LabelValuesHash {
    std::size_t operator()(const LabelValues& lv) const noexcept;
};

// ---- Families ----------------------------------------------------------

// One Family<T> holds all metrics of a single (name, type) — e.g. all
// label-value variants of "nl_receiver_associations_total". The unlabeled
// case is a Family with zero label names; labels({}) returns the singleton.
template <typename T>
class Family {
public:
    Family(std::string name, std::string help, std::vector<std::string> label_names)
        : name_(std::move(name)), help_(std::move(help)),
          label_names_(std::move(label_names)) {}

    virtual ~Family() = default;

    // labels({"v1","v2",...}) — returns or creates the metric for these
    // label values. Order must match label_names provided at construction.
    T& labels(const std::vector<std::string>& values);

    // Convenience for unlabeled families.
    T& self() { return labels({}); }

    const std::string& name() const noexcept { return name_; }
    const std::string& help() const noexcept { return help_; }
    const std::vector<std::string>& label_names() const noexcept { return label_names_; }

    // For exposition: visit all (label_values, metric*) pairs under a
    // shared lock.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        std::shared_lock lk(mutex_);
        for (const auto& [lv, metric] : metrics_) fn(lv, *metric);
    }

protected:
    // Subclasses override to provide construction kwargs (e.g. histogram
    // bucket bounds).
    virtual std::unique_ptr<T> make_metric() = 0;

private:
    std::string name_;
    std::string help_;
    std::vector<std::string> label_names_;

    mutable std::shared_mutex mutex_;
    std::unordered_map<LabelValues, std::unique_ptr<T>, LabelValuesHash> metrics_;
};

// Concrete family specializations.
class CounterFamily final : public Family<Counter> {
public:
    using Family::Family;
protected:
    std::unique_ptr<Counter> make_metric() override {
        return std::make_unique<Counter>();
    }
};

class GaugeFamily final : public Family<Gauge> {
public:
    using Family::Family;
protected:
    std::unique_ptr<Gauge> make_metric() override {
        return std::make_unique<Gauge>();
    }
};

class HistogramFamily final : public Family<Histogram> {
public:
    HistogramFamily(std::string name, std::string help,
                    std::vector<std::string> label_names,
                    std::vector<double>       bucket_bounds)
        : Family<Histogram>(std::move(name), std::move(help), std::move(label_names)),
          bucket_bounds_(std::move(bucket_bounds)) {}

    const std::vector<double>& bucket_bounds() const noexcept { return bucket_bounds_; }

protected:
    std::unique_ptr<Histogram> make_metric() override {
        return std::make_unique<Histogram>(bucket_bounds_);
    }

private:
    std::vector<double> bucket_bounds_;
};

// ---- Registry ----------------------------------------------------------

// Owns the set of families for a process. Most callers use the global()
// singleton; tests construct local Registry instances.
class Registry {
public:
    Registry()  = default;
    ~Registry() = default;

    Registry(const Registry&)            = delete;
    Registry& operator=(const Registry&) = delete;

    // Get or create a family. Idempotent for the same (name, type,
    // label_names) triple; throws std::logic_error on shape mismatch.
    CounterFamily&   counter(const std::string& name,
                              const std::string& help,
                              const std::vector<std::string>& label_names = {});
    GaugeFamily&     gauge(const std::string& name,
                            const std::string& help,
                            const std::vector<std::string>& label_names = {});
    HistogramFamily& histogram(const std::string& name,
                                const std::string& help,
                                const std::vector<double>& bucket_bounds,
                                const std::vector<std::string>& label_names = {});

    // Render in Prometheus text-exposition format (version 0.0.4).
    void render(std::ostream& os) const;
    std::string render() const;

    // Process-wide singleton. Lazy-initialized on first use.
    static Registry& global();

private:
    // We hold base-pointer ownership but expose typed references via the
    // accessor methods; a runtime check enforces type consistency for
    // duplicate registrations.
    enum class Kind { Counter, Gauge, Histogram };
    struct Entry {
        Kind kind;
        std::shared_ptr<void> family;     // owns the typed family
        std::string help;                  // for shape-mismatch errors
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace nlr::metrics
