# nl-dcm-probe — DICOM C-ECHO helper binary (M31).
#
# Statically links against the same DCMTK as the daemons (built by
# the package container or installed by apt during host tests).
# Used by the management API's destination test-connection feature
# to do a real DIMSE-level probe of `dicom`-kind destinations,
# replacing M19's TCP-only check.
#
# Shipped at /usr/libexec/nl-router/nl-dcm-probe in the .deb,
# alongside nl-dsl-validate. The Python API shells out to it; see
# python/nl_router/api/probes.py for the wrapper.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found (needed for nl-dcm-probe).")
endif()

# Deliberately small — argv-only, no JSON parser. Keeps the binary's
# dep surface to DCMTK plus stdlib, so it can live under common/ and
# get built before router/ runs FetchContent for nlohmann_json.
add_executable(nl-dcm-probe
    tools/dcm_probe.cpp
)

target_link_libraries(nl-dcm-probe PRIVATE
    DCMTK::DCMTK
)

target_compile_options(nl-dcm-probe PRIVATE
    # DCMTK headers trigger these on modern compilers; not actionable
    # from our side.
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
