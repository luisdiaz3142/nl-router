#include "config.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace nlr {

namespace {

// Read an environment variable; return nullopt if unset. We use a sentinel
// empty string to mean "unset" because std::getenv distinguishes nullptr
// from "" but most config loads treat both the same.
std::string env_or(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name)) {
        if (*v != '\0') return std::string{v};
    }
    return fallback;
}

std::string env_required(const char* name, const char* alt = nullptr) {
    if (const char* v = std::getenv(name)) {
        if (*v != '\0') return std::string{v};
    }
    if (alt) {
        if (const char* v = std::getenv(alt)) {
            if (*v != '\0') return std::string{v};
        }
    }
    throw std::runtime_error(
        std::string{"required env var not set: "} + name +
        (alt ? std::string{" (or "} + alt + ")" : ""));
}

template <typename T>
T env_int(const char* name, T fallback) {
    const char* v = std::getenv(name);
    if (!v || *v == '\0') return fallback;
    try {
        const long long parsed = std::stoll(v);
        return static_cast<T>(parsed);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string{"invalid integer for "} + name + ": " + v);
    }
}

}  // namespace

Config load_config() {
    Config c;
    c.server_id    = env_required("NL_ROUTER_SERVER_ID");
    c.database_url = env_required("NL_ROUTER_DATABASE_URL", "DATABASE_URL");

    c.host_aet               = env_or("NL_ROUTER_HOST_AET", c.host_aet);
    c.listen_port            = env_int<std::uint16_t>("NL_ROUTER_LISTEN_PORT", c.listen_port);
    c.landing_zone           = env_or("NL_ROUTER_LANDING_ZONE", c.landing_zone);
    c.max_pdu_size           = env_int<std::uint32_t>("NL_ROUTER_MAX_PDU_SIZE", c.max_pdu_size);
    c.max_associations       = env_int<std::uint16_t>("NL_ROUTER_MAX_ASSOCIATIONS",
                                                       c.max_associations);
    c.association_timeout_s  = env_int<std::uint32_t>("NL_ROUTER_ASSOCIATION_TIMEOUT_S",
                                                       c.association_timeout_s);
    c.metrics_port           = env_int<std::uint16_t>("NL_ROUTER_METRICS_PORT", c.metrics_port);
    c.metrics_bind_addr      = env_or("NL_ROUTER_METRICS_BIND_ADDR", c.metrics_bind_addr);
    c.disk_poll_interval_s   = env_int<std::uint32_t>("NL_ROUTER_DISK_POLL_INTERVAL_S",
                                                       c.disk_poll_interval_s);
    c.disk_warn_pct          = env_int<std::uint8_t>("NL_ROUTER_DISK_WARN_PCT",
                                                      c.disk_warn_pct);
    c.disk_reject_pct        = env_int<std::uint8_t>("NL_ROUTER_DISK_REJECT_PCT",
                                                      c.disk_reject_pct);

    // TLS knobs. tls_enabled accepts the usual truthy spellings.
    if (const char* v = std::getenv("NL_ROUTER_TLS_ENABLED"); v && *v) {
        const std::string s{v};
        c.tls_enabled = (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on");
    }
    c.tls_listen_port         = env_int<std::uint16_t>("NL_ROUTER_TLS_LISTEN_PORT",
                                                        c.tls_listen_port);
    c.tls_cert_file           = env_or("NL_ROUTER_TLS_CERT_FILE", c.tls_cert_file);
    c.tls_key_file            = env_or("NL_ROUTER_TLS_KEY_FILE",  c.tls_key_file);
    c.tls_ca_file             = env_or("NL_ROUTER_TLS_CA_FILE",   c.tls_ca_file);
    if (const char* v = std::getenv("NL_ROUTER_TLS_REQUIRE_CLIENT_CERT"); v && *v) {
        const std::string s{v};
        c.tls_require_client_cert =
            (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on");
    }
    c.tls_profile             = env_or("NL_ROUTER_TLS_PROFILE", c.tls_profile);

    c.log_level              = env_or("NL_ROUTER_LOG_LEVEL", c.log_level);
    return c;
}

}  // namespace nlr
