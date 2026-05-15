# nl_router_metrics
#
# Minimal Prometheus-compatible metrics primitives + text-exposition HTTP
# server. Header-only API; impl in two TUs. Linked by every service that
# exposes a /metrics endpoint (receiver in M10, others in follow-ups).

add_library(nl_router_metrics STATIC
    src/registry.cpp
    src/exposer.cpp
)

target_include_directories(nl_router_metrics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(nl_router_metrics PUBLIC cxx_std_20)

if(NL_ROUTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
