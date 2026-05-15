# Unit tests for nl_router_metrics. Reuses the Catch2 target already
# brought in by cpp/common/dsl/tests; the FetchContent_Declare there
# guarantees Catch2 is available before this file is processed.

# Catch2 is provided by the dsl tests subdirectory which is processed
# earlier (cpp/common/CMakeLists.txt adds dsl before metrics).
add_executable(nl_router_metrics_tests
    test_registry.cpp
)

target_link_libraries(nl_router_metrics_tests PRIVATE
    nl_router_metrics
    Catch2::Catch2WithMain
)

target_compile_options(nl_router_metrics_tests PRIVATE
    -Wno-non-virtual-dtor
    -Wno-sign-conversion
    -Wno-conversion
)

catch_discover_tests(nl_router_metrics_tests)
