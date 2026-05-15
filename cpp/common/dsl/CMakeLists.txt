# nl_router_dsl — rule expression parser and evaluator.
#
# Implements the dicomdiablo-compatible rule grammar:
#     tags.Modality == "CT" and float(tags.SliceThickness) < 2.0
#
# Parser: PEGTL (header-only PEG library). Evaluator: hand-written tree walker.
# Sandboxed by construction — only what's in the grammar runs.

include(FetchContent)

# ---- PEGTL (taocpp/PEGTL) -----------------------------------------------
# Header-only PEG parser library. Pinned to a known-stable release.
FetchContent_Declare(
    pegtl
    GIT_REPOSITORY https://github.com/taocpp/PEGTL.git
    GIT_TAG        3.2.7
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
# Skip PEGTL's own tests/examples in our build.
set(PEGTL_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(PEGTL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pegtl)

# ---- nl_router_dsl library ----------------------------------------------
add_library(nl_router_dsl
    src/value.cpp
    src/context.cpp
    src/parser.cpp
    src/evaluator.cpp
)
add_library(nl_router::dsl ALIAS nl_router_dsl)

target_include_directories(nl_router_dsl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_include_directories(nl_router_dsl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(nl_router_dsl PRIVATE taocpp::pegtl)

# ---- Tests --------------------------------------------------------------
if(NL_ROUTER_BUILD_TESTS)
    add_subdirectory(tests)
endif()
