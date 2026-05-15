# nl-receiver: DICOM SCP daemon.
#
# Listens on the configured DICOM port, accepts C-ECHO and C-STORE requests,
# writes received instances to the per-node landing zone, extracts tags, and
# inserts one work_queue row per study at association end.
#
# v1 (M1): single-threaded SCP, association-end close trigger, baseline tag
# set. Bounded thread pool, study/series idle timers, TLS, and Prometheus
# metrics land in follow-up commits.

include(FindDCMTK)
if(NOT DCMTK_FOUND)
    message(FATAL_ERROR "DCMTK not found. On macOS: brew install dcmtk")
endif()

# DCMTK 3.6.9+ adds:
#   - DcmTLSSecurityProfile enum value TSP_Profile_BCP_195_RFC_8996
#   - 3-arg overload of DcmTLSTransportLayer::setCertificateFile(file, type, profile)
# Older versions (e.g. Debian 12's 3.6.7) lack both. Detect at configure
# time and pass preprocessor macros so the TLS layer code can branch.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${DCMTK_INCLUDE_DIRS})
check_cxx_source_compiles("
    #include <dcmtk/dcmtls/tlsciphr.h>
    int main() { return (int)TSP_Profile_BCP_195_RFC_8996; }
" NL_RECEIVER_HAS_BCP195_RFC8996)
check_cxx_source_compiles("
    #include <dcmtk/dcmtls/tlslayer.h>
    int main() {
        DcmTLSTransportLayer tls(NET_ACCEPTOR, nullptr, OFTrue);
        tls.setCertificateFile(\"\", DCF_Filetype_PEM, TSP_Profile_BCP195_ND);
        return 0;
    }
" NL_RECEIVER_HAS_SETCERT_PROFILE)
unset(CMAKE_REQUIRED_INCLUDES)

# libpq (Postgres C client). On Homebrew it's keg-only at /opt/homebrew/opt/libpq.
find_package(PostgreSQL QUIET)
if(NOT PostgreSQL_FOUND)
    # Manual Homebrew fallback.
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

add_executable(nl-receiver
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/storage.cpp
    src/tag_extractor.cpp
    src/db.cpp
    src/association_handler.cpp
    src/server.cpp
    src/metrics.cpp
    src/disk_guard.cpp
    src/tls_layer.cpp
)

target_include_directories(nl-receiver PRIVATE
    src
    ${PostgreSQL_INCLUDE_DIRS}
)

target_link_libraries(nl-receiver PRIVATE
    DCMTK::DCMTK
    ${PostgreSQL_LIBRARIES}
    nl_router_metrics
)

if(NL_RECEIVER_HAS_BCP195_RFC8996)
    target_compile_definitions(nl-receiver PRIVATE NL_RECEIVER_HAS_BCP195_RFC8996=1)
endif()
if(NL_RECEIVER_HAS_SETCERT_PROFILE)
    target_compile_definitions(nl-receiver PRIVATE NL_RECEIVER_HAS_SETCERT_PROFILE=1)
endif()

# DCMTK's headers trip -Wold-style-cast / -Wsign-conversion. They're not our
# code; silence the noise on this target so we still see our own warnings.
target_compile_options(nl-receiver PRIVATE
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-conversion
    -Wno-shadow
)
