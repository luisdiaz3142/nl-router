# Common library targets shared across nl-router C++ services.

add_subdirectory(dsl)
add_subdirectory(crypto)
add_subdirectory(metrics)
# nl-dcm-probe — small DICOM C-ECHO CLI used by the management API's
# destination test-connection feature (M31). Links against DCMTK
# like the other DICOM-touching binaries.
add_subdirectory(dcm-probe)
