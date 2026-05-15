# nl-clean: cleaner daemon.
#
# Periodically scans work_queue on this node for terminal-status rows
# (dispatched / dispatched_partial / failed) whose retention TTL has
# elapsed, deletes their on-disk files, and marks the rows 'cleaned'.
# Row pruning (DELETE of long-cleaned rows) runs only on the cleaner
# instance that wins a Postgres advisory lock — self-healing leader
# election across nodes.
#
# v1 (M9) uses global retention defaults from system_config. Per-rule
# retention_override resolution lands as a follow-up.

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

add_executable(nl-clean
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/disk.cpp
    src/db.cpp
    src/server.cpp
)

target_include_directories(nl-clean PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(nl-clean PRIVATE
    ${PostgreSQL_LIBRARIES}
)

target_compile_options(nl-clean PRIVATE
    -Wno-shadow
    -Wno-sign-conversion
    -Wno-conversion
)
