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

    c.poll_interval_ms     = env_int<std::uint32_t>("NL_ROUTER_POLL_INTERVAL_MS", c.poll_interval_ms);
    c.batch_size           = env_int<std::uint32_t>("NL_ROUTER_BATCH_SIZE", c.batch_size);
    c.lease_seconds        = env_int<std::uint32_t>("NL_ROUTER_LEASE_SECONDS", c.lease_seconds);
    c.rule_cache_refresh_s = env_int<std::uint32_t>("NL_ROUTER_RULE_CACHE_REFRESH_S",
                                                     c.rule_cache_refresh_s);
    c.log_level            = env_or("NL_ROUTER_LOG_LEVEL", c.log_level);
    return c;
}

}  // namespace nlr
