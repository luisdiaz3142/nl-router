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

# libcurl >= 7.75.0 is required for CURLOPT_AWS_SIGV4 (used by the
# object_storage handler). On macOS the Apple SDK ships older curl
# headers even when the runtime libcurl is current — point CMake at
# the Homebrew curl explicitly when it's available.
if(APPLE)
    foreach(_curl_prefix /opt/homebrew/opt/curl /usr/local/opt/curl)
        if(EXISTS "${_curl_prefix}/include/curl/curl.h")
            set(CURL_ROOT "${_curl_prefix}" CACHE PATH "Homebrew curl root")
            list(APPEND CMAKE_PREFIX_PATH "${_curl_prefix}")
            break()
        endif()
    endforeach()
endif()
find_package(CURL 7.75.0 REQUIRED)

add_executable(nl-dispatch
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/db.cpp
    src/credential.cpp
    src/dicom_handler.cpp
    src/file_handler.cpp
    src/http_handler.cpp
    src/stow_upload.cpp
    src/dicomweb_stow_handler.cpp
    src/gcp_oauth.cpp
    src/gcp_dicomweb_handler.cpp
    src/object_storage_handler.cpp
    src/factory.cpp
    src/template.cpp
    src/worker.cpp
    src/server.cpp
    src/metrics.cpp
)

target_include_directories(nl-dispatch PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

# Apple's SDK has an older curl/curl.h that lacks CURLOPT_AWS_SIGV4; force
# the find_package result (brewed curl) ahead of -isysroot's lookups.
if(APPLE AND CURL_INCLUDE_DIRS)
    target_include_directories(nl-dispatch BEFORE PRIVATE ${CURL_INCLUDE_DIRS})
endif()

target_link_libraries(nl-dispatch PRIVATE
    DCMTK::DCMTK
    nl_router::crypto
    nl_router_metrics
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
