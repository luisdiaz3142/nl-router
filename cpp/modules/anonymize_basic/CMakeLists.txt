# nl-mod-anonymize-basic
#
# Strips and replaces DICOM tags per the per-rule config. The first
# concrete processing module shipped with nl-router; serves as the
# reference implementation for the worker-runtime contract.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found. On macOS: brew install dcmtk")
endif()

# nlohmann/json is fetched once by the router/dispatcher; reuse the
# global target if present.
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

add_executable(nl-mod-anonymize-basic
    src/main.cpp
)

target_link_libraries(nl-mod-anonymize-basic PRIVATE
    nl_router::module
    DCMTK::DCMTK
    nlohmann_json::nlohmann_json
)

target_compile_options(nl-mod-anonymize-basic PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
