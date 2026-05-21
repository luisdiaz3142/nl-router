# nl-mod-standardize-institution-group
#
# Single-purpose tag setter: writes (0008,1040) InstitutionalDepartmentName
# on every instance going through the chain. Config defaults the value to
# "NVRA"; per-rule overrides supported. See src/main.cpp for the rationale
# behind splitting this from anonymize_basic.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found. On macOS: brew install dcmtk")
endif()

# nlohmann/json: reuse the global target if it already exists (any sibling
# target — router, dispatcher, anonymize_basic — pulled it in first).
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

add_executable(nl-mod-standardize-institution-group
    src/main.cpp
)

target_link_libraries(nl-mod-standardize-institution-group PRIVATE
    nl_router::module
    DCMTK::DCMTK
    nlohmann_json::nlohmann_json
)

# DCMTK's headers trip our pedantic warnings; silence them on this target
# so we still see warnings on our own code.
target_compile_options(nl-mod-standardize-institution-group PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
