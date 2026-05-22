# nl-route: routing rule evaluator daemon.
#
# Polls work_queue for rows in status='received' on this node, evaluates
# the enabled rules' DSL predicates against each row's tags, and writes
# route_assignments rows for each matching (rule, destination) pair. Each
# work_queue row advances to status='routed' (or 'failed' on rule errors).
#
# This wires together:
#   * nl_router::dsl  — parse + evaluate routing predicates
#   * libpq           — claim rows / insert assignments / status transitions
#   * nlohmann/json   — parse the work_queue.tags JSONB column

include(FetchContent)

# ---- nlohmann/json (header-only) ---------------------------------------
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ---- libpq -------------------------------------------------------------
find_package(PostgreSQL QUIET)
if(NOT PostgreSQL_FOUND)
    find_path(PostgreSQL_INCLUDE_DIRS
        NAMES libpq-fe.h
        HINTS /opt/homebrew/opt/libpq/include /usr/local/opt/libpq/include
        REQUIRED
    )
    find_library(PostgreSQL_LIBRARIES
        NAMES pq
        HINTS /opt/homebrew/opt/libpq/lib /usr/local/opt/libpq/lib
        REQUIRED
    )
endif()

# ---- nl-route binary ---------------------------------------------------
add_executable(nl-route
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/db.cpp
    src/rule_cache.cpp
    src/context_builder.cpp
    src/server.cpp
    src/metrics.cpp
)

target_include_directories(nl-route PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(nl-route PRIVATE
    nl_router::dsl
    nl_router_metrics
    nlohmann_json::nlohmann_json
    ${PostgreSQL_LIBRARIES}
)

# nlohmann::json's templated code trips some of our pedantic conversion
# warnings; they aren't actionable from our side.
target_compile_options(nl-route PRIVATE
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
