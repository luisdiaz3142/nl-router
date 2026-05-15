#include "nl_router/metrics/registry.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace nlr::metrics {

// ---- Histogram ----------------------------------------------------------

Histogram::Histogram(std::vector<double> bucket_bounds)
    : bounds_(std::move(bucket_bounds))
{
    // Bounds must be strictly increasing. We don't sort silently — a
    // misordered bucket list is almost always a programming error.
    if (!std::is_sorted(bounds_.begin(), bounds_.end(),
                        [](double a, double b) { return a < b; })) {
        throw std::logic_error("Histogram bounds must be strictly increasing");
    }
    buckets_.reserve(bounds_.size());
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        buckets_.push_back(std::make_unique<std::atomic<std::int64_t>>(0));
    }
}

void Histogram::observe(double v) noexcept {
    // Find the smallest upper bound >= v. Each higher bucket is cumulative,
    // so we increment its counter and every wider one. Buckets are sorted,
    // so a linear scan is fine for the typical 10-bucket case; binary
    // search would micro-optimize for histograms with hundreds of buckets
    // (we don't ship any).
    for (std::size_t i = 0; i < bounds_.size(); ++i) {
        if (v <= bounds_[i]) {
            buckets_[i]->fetch_add(1, std::memory_order_relaxed);
        }
    }
    count_.fetch_add(1, std::memory_order_relaxed);

    // Sum is a double; atomic<double>::fetch_add isn't portable, so we CAS
    // on uint64 bits. Hot enough to matter — keep the loop tight.
    std::uint64_t expected = sum_bits_.load(std::memory_order_relaxed);
    while (true) {
        double current;
        std::memcpy(&current, &expected, sizeof(double));
        const double next = current + v;
        std::uint64_t next_bits;
        std::memcpy(&next_bits, &next, sizeof(double));
        if (sum_bits_.compare_exchange_weak(expected, next_bits,
                                             std::memory_order_relaxed)) {
            break;
        }
    }
}

std::vector<std::int64_t> Histogram::bucket_counts() const {
    std::vector<std::int64_t> out;
    out.reserve(buckets_.size());
    for (const auto& b : buckets_) {
        out.push_back(b->load(std::memory_order_relaxed));
    }
    return out;
}

double Histogram::sum() const noexcept {
    const std::uint64_t bits = sum_bits_.load(std::memory_order_relaxed);
    double v;
    std::memcpy(&v, &bits, sizeof(double));
    return v;
}

// ---- LabelValues hash ---------------------------------------------------

std::size_t LabelValuesHash::operator()(const LabelValues& lv) const noexcept {
    // FNV-1a over each value with a separator. Cheap and good enough for
    // the cardinalities we expect (single digits to low hundreds per family).
    std::size_t h = 1469598103934665603ull;
    for (const auto& s : lv.values) {
        for (char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ull;
        }
        h ^= 0xff;       // separator between values
        h *= 1099511628211ull;
    }
    return h;
}

// ---- Family<T>::labels ---------------------------------------------------

template <typename T>
T& Family<T>::labels(const std::vector<std::string>& values) {
    if (values.size() != label_names_.size()) {
        throw std::logic_error(
            "Family '" + name_ + "': label arity mismatch (expected " +
            std::to_string(label_names_.size()) + ", got " +
            std::to_string(values.size()) + ")");
    }

    LabelValues key{values};

    // Fast path: shared lock, lookup, return.
    {
        std::shared_lock lk(mutex_);
        auto it = metrics_.find(key);
        if (it != metrics_.end()) return *it->second;
    }
    // Slow path: upgrade to unique, double-check, insert.
    std::unique_lock lk(mutex_);
    auto it = metrics_.find(key);
    if (it != metrics_.end()) return *it->second;
    auto metric = make_metric();
    T* raw = metric.get();
    metrics_.emplace(std::move(key), std::move(metric));
    return *raw;
}

// Explicit instantiations to keep the implementation out of the header.
template class Family<Counter>;
template class Family<Gauge>;
template class Family<Histogram>;

// ---- Registry -----------------------------------------------------------

CounterFamily& Registry::counter(const std::string& name,
                                  const std::string& help,
                                  const std::vector<std::string>& label_names) {
    {
        std::shared_lock lk(mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            if (it->second.kind != Kind::Counter) {
                throw std::logic_error("metric '" + name +
                    "' already registered with a different type");
            }
            return *std::static_pointer_cast<CounterFamily>(it->second.family);
        }
    }
    std::unique_lock lk(mutex_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        if (it->second.kind != Kind::Counter) {
            throw std::logic_error("metric '" + name +
                "' already registered with a different type");
        }
        return *std::static_pointer_cast<CounterFamily>(it->second.family);
    }
    auto fam = std::make_shared<CounterFamily>(name, help, label_names);
    entries_.emplace(name, Entry{Kind::Counter, fam, help});
    return *fam;
}

GaugeFamily& Registry::gauge(const std::string& name,
                              const std::string& help,
                              const std::vector<std::string>& label_names) {
    {
        std::shared_lock lk(mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            if (it->second.kind != Kind::Gauge) {
                throw std::logic_error("metric '" + name +
                    "' already registered with a different type");
            }
            return *std::static_pointer_cast<GaugeFamily>(it->second.family);
        }
    }
    std::unique_lock lk(mutex_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        if (it->second.kind != Kind::Gauge) {
            throw std::logic_error("metric '" + name +
                "' already registered with a different type");
        }
        return *std::static_pointer_cast<GaugeFamily>(it->second.family);
    }
    auto fam = std::make_shared<GaugeFamily>(name, help, label_names);
    entries_.emplace(name, Entry{Kind::Gauge, fam, help});
    return *fam;
}

HistogramFamily& Registry::histogram(const std::string& name,
                                      const std::string& help,
                                      const std::vector<double>& bucket_bounds,
                                      const std::vector<std::string>& label_names) {
    {
        std::shared_lock lk(mutex_);
        auto it = entries_.find(name);
        if (it != entries_.end()) {
            if (it->second.kind != Kind::Histogram) {
                throw std::logic_error("metric '" + name +
                    "' already registered with a different type");
            }
            return *std::static_pointer_cast<HistogramFamily>(it->second.family);
        }
    }
    std::unique_lock lk(mutex_);
    auto it = entries_.find(name);
    if (it != entries_.end()) {
        if (it->second.kind != Kind::Histogram) {
            throw std::logic_error("metric '" + name +
                "' already registered with a different type");
        }
        return *std::static_pointer_cast<HistogramFamily>(it->second.family);
    }
    auto fam = std::make_shared<HistogramFamily>(name, help, label_names, bucket_bounds);
    entries_.emplace(name, Entry{Kind::Histogram, fam, help});
    return *fam;
}

// ---- Render -------------------------------------------------------------

namespace {

// Prometheus text-exposition escapes:
//   * label value: " → \", \\ → \\\\, \n → \\n
//   * help text:   \\ → \\\\, \n → \\n
std::string escape_label_value(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
    return out;
}

std::string escape_help(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if      (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
    return out;
}

// Render a label set for one metric line:
//   {name="v1",foo="v2"}
// Returns empty string for zero-label families.
std::string render_label_set(const std::vector<std::string>& names,
                              const LabelValues& values,
                              const std::string& extra_name = "",
                              const std::string& extra_value = "")
{
    if (names.empty() && extra_name.empty()) return {};
    std::string out = "{";
    bool first = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!first) out += ',';
        out += names[i];
        out += "=\"";
        out += escape_label_value(values.values[i]);
        out += "\"";
        first = false;
    }
    if (!extra_name.empty()) {
        if (!first) out += ',';
        out += extra_name;
        out += "=\"";
        out += escape_label_value(extra_value);
        out += "\"";
    }
    out += "}";
    return out;
}

// Format a double for Prometheus exposition. Per the format spec, +Inf,
// -Inf, NaN are special-cased; otherwise we emit the shortest decimal
// representation that round-trips back to the same double. std::to_chars
// gives that exactly (C++17 §20.19.2 "shortest" overload), avoiding the
// 17-digit "0.10000000000000001" noise we'd get from streaming with
// max_digits10.
std::string fmt_double(double v) {
    if (std::isnan(v))      return "NaN";
    if (v ==  std::numeric_limits<double>::infinity()) return "+Inf";
    if (v == -std::numeric_limits<double>::infinity()) return "-Inf";
    std::array<char, 32> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v);
    if (res.ec != std::errc{}) {
        // Fallback shouldn't trigger for any finite double; preserve safety.
        std::ostringstream oss;
        oss.precision(std::numeric_limits<double>::max_digits10);
        oss << v;
        return oss.str();
    }
    return std::string(buf.data(), static_cast<std::size_t>(res.ptr - buf.data()));
}

void render_counter_family(std::ostream& os, const CounterFamily& fam) {
    os << "# HELP " << fam.name() << ' ' << escape_help(fam.help()) << '\n';
    os << "# TYPE " << fam.name() << " counter\n";
    fam.for_each([&](const LabelValues& lv, const Counter& c) {
        os << fam.name()
           << render_label_set(fam.label_names(), lv)
           << ' ' << c.get() << '\n';
    });
}

void render_gauge_family(std::ostream& os, const GaugeFamily& fam) {
    os << "# HELP " << fam.name() << ' ' << escape_help(fam.help()) << '\n';
    os << "# TYPE " << fam.name() << " gauge\n";
    fam.for_each([&](const LabelValues& lv, const Gauge& g) {
        os << fam.name()
           << render_label_set(fam.label_names(), lv)
           << ' ' << g.get() << '\n';
    });
}

void render_histogram_family(std::ostream& os, const HistogramFamily& fam) {
    os << "# HELP " << fam.name() << ' ' << escape_help(fam.help()) << '\n';
    os << "# TYPE " << fam.name() << " histogram\n";
    fam.for_each([&](const LabelValues& lv, const Histogram& h) {
        const auto counts = h.bucket_counts();
        for (std::size_t i = 0; i < fam.bucket_bounds().size(); ++i) {
            os << fam.name() << "_bucket"
               << render_label_set(fam.label_names(), lv,
                                    "le", fmt_double(fam.bucket_bounds()[i]))
               << ' ' << counts[i] << '\n';
        }
        // +Inf bucket: total count.
        os << fam.name() << "_bucket"
           << render_label_set(fam.label_names(), lv, "le", "+Inf")
           << ' ' << h.count() << '\n';
        os << fam.name() << "_sum"
           << render_label_set(fam.label_names(), lv)
           << ' ' << fmt_double(h.sum()) << '\n';
        os << fam.name() << "_count"
           << render_label_set(fam.label_names(), lv)
           << ' ' << h.count() << '\n';
    });
}

}  // namespace

void Registry::render(std::ostream& os) const {
    // Sort entries by name for deterministic output. Prometheus doesn't
    // require ordering but tests and humans appreciate it.
    std::vector<std::pair<std::string, Entry>> sorted;
    {
        std::shared_lock lk(mutex_);
        sorted.reserve(entries_.size());
        for (const auto& kv : entries_) sorted.emplace_back(kv);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [name, entry] : sorted) {
        switch (entry.kind) {
            case Kind::Counter:
                render_counter_family(os,
                    *std::static_pointer_cast<CounterFamily>(entry.family));
                break;
            case Kind::Gauge:
                render_gauge_family(os,
                    *std::static_pointer_cast<GaugeFamily>(entry.family));
                break;
            case Kind::Histogram:
                render_histogram_family(os,
                    *std::static_pointer_cast<HistogramFamily>(entry.family));
                break;
        }
    }
}

std::string Registry::render() const {
    std::ostringstream oss;
    render(oss);
    return oss.str();
}

Registry& Registry::global() {
    static Registry instance;
    return instance;
}

}  // namespace nlr::metrics
