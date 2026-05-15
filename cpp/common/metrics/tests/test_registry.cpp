// Unit tests for nlr::metrics::Registry and its primitives.
//
// We don't test the HTTP exposer here — it's a thin transport over
// Registry::render(). Render-shape tests cover the text-exposition format,
// which is what scrape clients actually consume.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "nl_router/metrics/registry.hpp"

using namespace nlr::metrics;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("Counter monotonic increment", "[metrics][counter]") {
    Counter c;
    REQUIRE(c.get() == 0);
    c.inc();
    c.inc(5);
    REQUIRE(c.get() == 6);
    // Negative deltas are ignored — counters are monotonic by spec.
    c.inc(-3);
    REQUIRE(c.get() == 6);
}

TEST_CASE("Gauge up/down/set", "[metrics][gauge]") {
    Gauge g;
    REQUIRE(g.get() == 0);
    g.set(42);
    REQUIRE(g.get() == 42);
    g.inc();
    REQUIRE(g.get() == 43);
    g.dec(10);
    REQUIRE(g.get() == 33);
}

TEST_CASE("Histogram observes into cumulative buckets", "[metrics][histogram]") {
    Histogram h{{0.1, 0.5, 1.0, 5.0}};

    h.observe(0.05);   // ≤ 0.1 → bucket 0,1,2,3
    h.observe(0.3);    // ≤ 0.5 → bucket 1,2,3
    h.observe(2.0);    // ≤ 5.0 → bucket 3
    h.observe(100.0);  // none of the bounded buckets, but count/+Inf

    REQUIRE(h.count() == 4);
    REQUIRE(h.sum() == Catch::Approx(102.35));

    const auto counts = h.bucket_counts();
    REQUIRE(counts.size() == 4);
    REQUIRE(counts[0] == 1);   // le 0.1
    REQUIRE(counts[1] == 2);   // le 0.5
    REQUIRE(counts[2] == 2);   // le 1.0
    REQUIRE(counts[3] == 3);   // le 5.0
}

TEST_CASE("Histogram rejects non-monotonic bounds", "[metrics][histogram]") {
    REQUIRE_THROWS_AS((Histogram{{1.0, 0.5}}), std::logic_error);
}

TEST_CASE("Registry counter family with labels", "[metrics][registry]") {
    Registry r;
    auto& fam = r.counter("test_counter", "Test counter", {"result"});
    fam.labels({"accepted"}).inc();
    fam.labels({"rejected"}).inc(3);
    fam.labels({"accepted"}).inc();
    REQUIRE(fam.labels({"accepted"}).get() == 2);
    REQUIRE(fam.labels({"rejected"}).get() == 3);
}

TEST_CASE("Registry idempotent registration", "[metrics][registry]") {
    Registry r;
    auto& a = r.counter("c", "help");
    auto& b = r.counter("c", "help");
    // Same family object returned.
    REQUIRE(&a == &b);
}

TEST_CASE("Registry rejects type mismatch", "[metrics][registry]") {
    Registry r;
    r.counter("x", "h");
    REQUIRE_THROWS_AS(r.gauge("x", "h"), std::logic_error);
}

TEST_CASE("Registry render emits text-exposition format", "[metrics][registry][render]") {
    Registry r;
    auto& c = r.counter("nl_test_total", "Total things", {"kind"});
    c.labels({"foo"}).inc(5);
    c.labels({"bar"}).inc(2);

    auto& g = r.gauge("nl_test_active", "Active things");
    g.self().set(7);

    auto& h = r.histogram("nl_test_dur_seconds", "Duration",
                          {0.1, 1.0, 10.0});
    h.self().observe(0.05);
    h.self().observe(0.5);
    h.self().observe(2.0);

    const std::string out = r.render();

    // Counter
    REQUIRE_THAT(out, ContainsSubstring("# HELP nl_test_total Total things"));
    REQUIRE_THAT(out, ContainsSubstring("# TYPE nl_test_total counter"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_total{kind=\"foo\"} 5"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_total{kind=\"bar\"} 2"));

    // Gauge
    REQUIRE_THAT(out, ContainsSubstring("# TYPE nl_test_active gauge"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_active 7"));

    // Histogram
    REQUIRE_THAT(out, ContainsSubstring("# TYPE nl_test_dur_seconds histogram"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_dur_seconds_bucket{le=\"0.1\"} 1"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_dur_seconds_bucket{le=\"1\"} 2"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_dur_seconds_bucket{le=\"+Inf\"} 3"));
    REQUIRE_THAT(out, ContainsSubstring("nl_test_dur_seconds_count 3"));
}

TEST_CASE("Render escapes label values and help", "[metrics][registry][render]") {
    Registry r;
    // Per Prometheus text-exposition: HELP escapes only `\\` and `\n`
    // (quotes are NOT escaped in HELP). Label values escape `\\`, `\n`,
    // and `"`.
    auto& c = r.counter("nl_esc_total",
        "back\\slash and a\nnewline", {"path"});
    c.labels({"a\"b\\c"}).inc();
    const std::string out = r.render();
    REQUIRE_THAT(out, ContainsSubstring("back\\\\slash and a\\nnewline"));
    REQUIRE_THAT(out, ContainsSubstring("path=\"a\\\"b\\\\c\""));
}

TEST_CASE("Label arity mismatch throws", "[metrics][registry]") {
    Registry r;
    auto& fam = r.counter("c", "h", {"a", "b"});
    REQUIRE_THROWS_AS(fam.labels({"only-one"}), std::logic_error);
}
