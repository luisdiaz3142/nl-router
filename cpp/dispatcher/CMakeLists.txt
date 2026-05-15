# nl-dispatch: outbound DICOM dispatcher daemon.
#
# Polls route_assignments for pending rows on this node, dispatches each to
# its destination (currently only kind='dicom'), and rolls up work_queue.status
# when all assignments for a row reach a terminal state.
#
# v1 (M3) handles only the 'dicom' kind (DCMTK DcmSCU). dicomweb_stow /
# gcp_dicomweb / http / file / object_storage handlers land in M7
# (Dispatcher M2) using the same DispatchHandler interface and worker contract.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found. On macOS: brew install dcmtk")
endif()

# libpq
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

# nlohmann/json was already fetched by the router target — reuse it.
# FetchContent's target is global once made available.
if(NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
    )
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(CURL REQUIRED)

add_executable(nl-dispatch
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/db.cpp
    src/dicom_handler.cpp
    src/file_handler.cpp
    src/http_handler.cpp
    src/factory.cpp
    src/template.cpp
    src/worker.cpp
    src/server.cpp
)

target_include_directories(nl-dispatch PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(nl-dispatch PRIVATE
    DCMTK::DCMTK
    nl_router::crypto
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${PostgreSQL_LIBRARIES}
)

target_compile_options(nl-dispatch PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
