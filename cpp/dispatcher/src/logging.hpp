// nl-receiver/logging.hpp
//
// Minimal structured logger. Emits JSON-lines to stderr (which systemd-journald
// captures by default, and which container runtimes pick up as the std stream).
//
// Levels: TRACE, DEBUG, INFO, WARN, ERROR. v1 exposes INFO and DEBUG via the
// config; the others are wired internally for future config promotion (per
// the design plan).
//
// We deliberately don't pull spdlog in v1 — fewer deps, smaller binary,
// trivial to audit. Once we add multi-process module workers and need
// async / rotating sinks, we'll swap to spdlog.
//
// Usage:
//     LOG_INFO("association.accept", "calling_aet", aet, "peer_ip", ip);
//     LOG_DEBUG("instance.write", "sop_uid", uid, "bytes", size);
//
// Each call emits one JSON object on stderr:
//     {"ts":"...","lvl":"info","msg":"association.accept",
//      "calling_aet":"MOD1","peer_ip":"10.0.0.5"}

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nlr::log {

enum class Level : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

// Process-global threshold. Set from config at startup; reads are atomic-ish
// (a relaxed integer load) and thread-safe enough for log filtering.
void set_level(Level lvl) noexcept;
Level get_level() noexcept;

// Parse "info" / "debug" / "warn" / "error" / "trace" → Level.
// Falls back to Info on unknown input.
Level parse_level(std::string_view s) noexcept;

// Low-level emission. `msg` is a short event name; `kv_pairs` are alternating
// key/value strings (must be even count). The function is a no-op when the
// configured level is above the call level.
void emit(Level lvl, std::string_view msg,
          std::initializer_list<std::string_view> kv_pairs);

}  // namespace nlr::log

// ---- Convenience macros -------------------------------------------------
//
// These take a message and zero-or-more key/value pairs. Values are converted
// via the to_string helpers below; everything ultimately becomes std::string.

// C++20 __VA_OPT__ avoids the GNU `, ##__VA_ARGS__` extension warning.
#define NLR_LOG_(LVL, MSG, ...)                                              \
    do {                                                                     \
        if (::nlr::log::get_level() <= (LVL)) {                              \
            ::nlr::log::emit((LVL), (MSG), { __VA_ARGS__ });                 \
        }                                                                    \
    } while (0)

#define LOG_TRACE(msg, ...) NLR_LOG_(::nlr::log::Level::Trace, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_DEBUG(msg, ...) NLR_LOG_(::nlr::log::Level::Debug, msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_INFO(msg, ...)  NLR_LOG_(::nlr::log::Level::Info,  msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_WARN(msg, ...)  NLR_LOG_(::nlr::log::Level::Warn,  msg __VA_OPT__(,) __VA_ARGS__)
#define LOG_ERROR(msg, ...) NLR_LOG_(::nlr::log::Level::Error, msg __VA_OPT__(,) __VA_ARGS__)
