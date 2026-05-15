# Top-level CMake for the C++ tree.
#
# Layout:
#   cpp/common/        shared libraries used by multiple services
#     dsl/             rule expression DSL (parser + evaluator)
#   cpp/receiver/      DICOM SCP daemon (M1, not yet implemented)
#   cpp/router/        rule evaluator binary (M2, not yet implemented)
#   cpp/dispatcher/    outbound sender (M3, not yet implemented)
#   cpp/cleaner/       cleanup daemon (M9, not yet implemented)
#   cpp/modules/       built-in processing modules (M6/M8)
#   cpp/getdcmtags/    tag extraction helper (M1)
#
# Only the targets that currently have source land below; others are stubs we
# will enable as their milestones are built out.

cmake_minimum_required(VERSION 3.21)
project(nl-router-cpp
    VERSION 0.0.1
    DESCRIPTION "nl-router native components"
    LANGUAGES CXX
)

# ---- C++ standard -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release builds when none specified; tests build under Debug by
# convention to keep assert() live.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Pedantic warnings everywhere; we treat warnings as errors in CI builds.
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wno-unused-parameter  # PEGTL state structs trip this in CI
)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    add_compile_options(-Wno-unused-but-set-variable)
endif()

# ---- Options ------------------------------------------------------------
option(NL_ROUTER_BUILD_TESTS "Build unit tests (Catch2)" ON)

if(NL_ROUTER_BUILD_TESTS)
    # Enables ctest at the top-level so subdirectory test registrations
    # (via catch_discover_tests) actually surface in `ctest --output-on-failure`.
    enable_testing()
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

# ---- Subdirectories -----------------------------------------------------
add_subdirectory(common)
add_subdirectory(receiver)
add_subdirectory(router)
add_subdirectory(dispatcher)

# Future:
#   add_subdirectory(getdcmtags)
#   add_subdirectory(cleaner)
#   add_subdirectory(modules)

# ---- Status summary -----------------------------------------------------
message(STATUS "")
message(STATUS "nl-router C++ build configuration")
message(STATUS "  CMAKE_BUILD_TYPE         : ${CMAKE_BUILD_TYPE}")
message(STATUS "  CMAKE_CXX_COMPILER       : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  NL_ROUTER_BUILD_TESTS    : ${NL_ROUTER_BUILD_TESTS}")
message(STATUS "")
