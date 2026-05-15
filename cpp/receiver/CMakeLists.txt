# nl-receiver: DICOM SCP daemon.
#
# Listens on the configured DICOM port, accepts C-ECHO and C-STORE requests,
# writes received instances to the per-node landing zone, extracts tags, and
# inserts one work_queue row per study at association end.
#
# v1 (M1): single-threaded SCP, association-end close trigger, baseline tag
# set. Bounded thread pool, study/series idle timers, TLS, and Prometheus
# metrics land in follow-up commits.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found. On macOS: brew install dcmtk")
endif()

# libpq (Postgres C client). On Homebrew it's keg-only at /opt/homebrew/opt/libpq.
find_package(PostgreSQL QUIET)
if(NOT PostgreSQL_FOUND)
    # Manual Homebrew fallback.
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

add_executable(nl-receiver
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/storage.cpp
    src/tag_extractor.cpp
    src/db.cpp
    src/association_handler.cpp
    src/server.cpp
    src/metrics.cpp
)

target_include_directories(nl-receiver PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(nl-receiver PRIVATE
    DCMTK::DCMTK
    ${PostgreSQL_LIBRARIES}
    nl_router_metrics
)

# DCMTK's headers trip -Wold-style-cast / -Wsign-conversion. They're not our
# code; silence the noise on this target so we still see our own warnings.
target_compile_options(nl-receiver PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
