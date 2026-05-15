#include "config.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace nlr {

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    return fallback;
}

std::string env_required(const char* name, const char* alt = nullptr) {
    if (const char* v = std::getenv(name); v && *v) return std::string{v};
    if (alt) {
        if (const char* v = std::getenv(alt); v && *v) return std::string{v};
    }
    throw std::runtime_error(
        std::string{"required env var not set: "} + name +
        (alt ? std::string{" (or "} + alt + ")" : ""));
}

template <typename T>
T env_int(const char* name, T fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return static_cast<T>(std::stoll(v));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string{"invalid integer for "} + name + ": " + v);
    }
}

}  // namespace

Config load_config() {
    Config c;
    c.server_id    = env_required("NL_ROUTER_SERVER_ID");
    c.database_url = env_required("NL_ROUTER_DATABASE_URL", "DATABASE_URL");

    c.landing_zone     = env_or("NL_ROUTER_LANDING_ZONE", c.landing_zone);
    c.scan_interval_s  = env_int<std::uint32_t>("NL_ROUTER_SCAN_INTERVAL_S", c.scan_interval_s);
    c.file_batch       = env_int<std::uint32_t>("NL_ROUTER_FILE_BATCH",      c.file_batch);
    c.prune_batch      = env_int<std::uint32_t>("NL_ROUTER_PRUNE_BATCH",     c.prune_batch);
    c.log_level        = env_or("NL_ROUTER_LOG_LEVEL", c.log_level);
    return c;
}

}  // namespace nlr
