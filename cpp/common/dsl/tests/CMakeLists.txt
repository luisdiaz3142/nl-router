# Unit tests for nl_router::dsl using Catch2 v3.

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(Catch2)

# Catch2 ships CMake helpers (catch_discover_tests) via its cmake dir.
list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(Catch)

add_executable(nl_router_dsl_tests
    test_value.cpp
    test_parser.cpp
    test_evaluator.cpp
)

target_link_libraries(nl_router_dsl_tests PRIVATE
    nl_router::dsl
    Catch2::Catch2WithMain
)

# Catch2's own headers trip -Wnon-virtual-dtor / -Wsign-conversion / etc.
# Those are issues for Catch2 to fix, not us. Suppress them on the test target.
target_compile_options(nl_router_dsl_tests PRIVATE
    -Wno-non-virtual-dtor
    -Wno-sign-conversion
    -Wno-conversion
)

# Register with ctest.
catch_discover_tests(nl_router_dsl_tests)
