#include "logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace nlr::log {

namespace {

// Global threshold. atomic for visibility across threads; level changes are
// rare so the relaxed semantics are fine.
std::atomic<int> g_level{static_cast<int>(Level::Info)};

// Stderr is line-buffered when attached to a tty but block-buffered when
// redirected; protect concurrent emissions with a mutex so journald gets one
// JSON object per line.
std::mutex& sink_mutex() {
    static std::mutex m;
    return m;
}

// ISO-8601 UTC timestamp with millisecond precision: 2026-05-15T02:46:12.345Z
std::string iso_now() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) - secs;

    const std::time_t t = clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S");
    oss << '.' << std::setw(3) << std::setfill('0') << ms.count() << 'Z';
    return oss.str();
}

const char* level_name(Level lvl) noexcept {
    switch (lvl) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "info";
}

// Minimal JSON string escaper. DICOM tag values can contain backslashes
// (multi-value separator), control chars, and the occasional double-quote.
void append_json_string(std::string& out, std::string_view s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

}  // namespace

void set_level(Level lvl) noexcept {
    g_level.store(static_cast<int>(lvl), std::memory_order_relaxed);
}

Level get_level() noexcept {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

Level parse_level(std::string_view s) noexcept {
    if (s == "trace") return Level::Trace;
    if (s == "debug") return Level::Debug;
    if (s == "info")  return Level::Info;
    if (s == "warn"  || s == "warning") return Level::Warn;
    if (s == "error" || s == "err")     return Level::Error;
    return Level::Info;
}

void emit(Level lvl, std::string_view msg,
          std::initializer_list<std::string_view> kv_pairs) {
    std::string line;
    line.reserve(128 + msg.size());

    line += "{\"ts\":\"";
    line += iso_now();
    line += "\",\"lvl\":\"";
    line += level_name(lvl);
    line += "\",\"msg\":";
    append_json_string(line, msg);

    auto it = kv_pairs.begin();
    while (it != kv_pairs.end()) {
        const auto& key = *it++;
        if (it == kv_pairs.end()) break;          // skip dangling key
        const auto& val = *it++;
        line.push_back(',');
        append_json_string(line, key);
        line.push_back(':');
        append_json_string(line, val);
    }

    line.push_back('}');
    line.push_back('\n');

    std::lock_guard<std::mutex> lock(sink_mutex());
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

}  // namespace nlr::log
