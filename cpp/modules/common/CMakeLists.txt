# nl_router_module — shared worker runtime for processing-module binaries.
#
# Each module binary (e.g. nl-mod-anonymize-basic) links this and supplies
# a single process(input, output, config_json) function. The library owns
# the queue loop, signal handling, Postgres transitions, and work_queue
# rollup.

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

add_library(nl_router_module STATIC
    src/worker.cpp
)
add_library(nl_router::module ALIAS nl_router_module)

target_include_directories(nl_router_module PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(nl_router_module PRIVATE
    ${PostgreSQL_INCLUDE_DIRS}
)
target_link_libraries(nl_router_module PUBLIC
    ${PostgreSQL_LIBRARIES}
)
