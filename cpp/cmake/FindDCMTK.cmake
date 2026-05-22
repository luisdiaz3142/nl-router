# Minimal DCMTK locator.
#
# Two modes:
#
#  * Dynamic (default): tries DCMTK's own CMake config first (works for
#    system installs that ship a DCMTKConfig.cmake), then falls back to
#    a manual search via headers + lib names. The fallback works on
#    Homebrew installs where DCMTK's bundled CMake package config can
#    lag behind library locations.
#
#  * Static (NL_ROUTER_STATIC_DCMTK=ON, set by the package build): skip
#    CMake's CONFIG mode and find each .a archive directly. Link via
#    --start-group/--end-group so ld can iteratively resolve the
#    intra-DCMTK symbol dependencies without us spelling out the exact
#    order. System libraries DCMTK depends on (zlib, openssl, pthread,
#    dl) stay dynamic — those have stable ABIs across the distro
#    versions we care about.
#
# After this module runs, the imported target `DCMTK::DCMTK` is available.
# Pass it to target_link_libraries() and it picks up include dirs and link
# libs automatically.

if(TARGET DCMTK::DCMTK)
    return()
endif()

# -------------------------------------------------------------------------
# Static path
# -------------------------------------------------------------------------
if(NL_ROUTER_STATIC_DCMTK)
    # Minimum set of DCMTK component libraries the nl-router binaries
    # touch. Ordering is hint-only — --start-group lets ld iterate
    # archives until all symbols resolve, so a near-miss here doesn't
    # break the link.
    #
    # If you add a daemon that uses, say, dcmsr (structured reports),
    # add the lib name here.
    set(_NLR_DCMTK_STATIC_LIBS
        # network + TLS
        dcmnet dcmtls
        # compressed transfer syntaxes (advertised during association)
        dcmjpeg ijg8 ijg12 ijg16
        dcmjpls dcmtkcharls
        # image / dataset
        dcmimage dcmimgle i2d dcmdata
        # foundation. oficonv is DCMTK's wrapper around iconv for
        # character-set conversion (SpecificCharacterSet tag handling);
        # libofstd references it, so it has to be inside the
        # --start-group window or the link fails with undefined
        # OFiconv_open / OFiconv_close.
        oflog oficonv ofstd
    )

    set(_archives "")
    foreach(_lib IN LISTS _NLR_DCMTK_STATIC_LIBS)
        # Cache var so re-configures don't keep paying the search cost.
        # Search /usr/local first (where Dockerfile.build installs our
        # from-source DCMTK), then the system multiarch dirs.
        find_library(NLR_DCMTK_STATIC_${_lib}
            NAMES lib${_lib}.a
            HINTS
                /usr/local/lib
                /usr/lib
                /usr/lib/x86_64-linux-gnu
                /usr/lib/aarch64-linux-gnu
        )
        if(NLR_DCMTK_STATIC_${_lib})
            list(APPEND _archives "${NLR_DCMTK_STATIC_${_lib}}")
        else()
            message(FATAL_ERROR
                "DCMTK static lib not found: lib${_lib}.a. "
                "The package build expects DCMTK compiled with "
                "BUILD_SHARED_LIBS=OFF in /usr/local — see "
                "packaging/Dockerfile.build. If you intentionally want "
                "dynamic linking, drop -DNL_ROUTER_STATIC_DCMTK=ON.")
        endif()
    endforeach()

    find_path(NLR_DCMTK_INCLUDE_DIR
        NAMES dcmtk/dcmnet/dimse.h
        HINTS /usr/local/include /usr/include
    )
    if(NOT NLR_DCMTK_INCLUDE_DIR)
        message(FATAL_ERROR
            "DCMTK headers not found. Install libdcmtk-dev "
            "(Debian/Ubuntu) or the equivalent on your distro.")
    endif()

    add_library(DCMTK::DCMTK INTERFACE IMPORTED)
    target_include_directories(DCMTK::DCMTK INTERFACE ${NLR_DCMTK_INCLUDE_DIR})

    # The --start-group / --end-group pair tells ld to keep iterating
    # the archives until it can resolve every undefined symbol — needed
    # because DCMTK components reference each other in both directions
    # (oflog uses ofstd, dcmdata uses oflog, dcmnet uses dcmdata, …).
    # System libs after the group stay dynamic.
    target_link_libraries(DCMTK::DCMTK INTERFACE
        "-Wl,--start-group"
        ${_archives}
        "-Wl,--end-group"
        # System deps DCMTK pulls in. Listed by name; CMake won't
        # resolve these as imported targets but the linker has them on
        # default search paths.
        ssl
        crypto
        z
        pthread
        dl
    )
    set(DCMTK_FOUND TRUE)
    return()
endif()

# -------------------------------------------------------------------------
# Dynamic path (default — dev builds)
# -------------------------------------------------------------------------

# Try the upstream CMake package first. Recent DCMTK versions (incl. Homebrew
# 3.6.9) ship DCMTKConfig.cmake which creates the DCMTK::DCMTK target itself.
find_package(DCMTK CONFIG QUIET)

# Case 1: the package already created the imported target. Nothing more to do.
if(TARGET DCMTK::DCMTK)
    return()
endif()

# Case 2: find_package found the variables but didn't create the target.
# Create it ourselves from the variables.
if(DCMTK_FOUND AND DCMTK_LIBRARIES)
    add_library(DCMTK::DCMTK INTERFACE IMPORTED)
    target_include_directories(DCMTK::DCMTK INTERFACE ${DCMTK_INCLUDE_DIRS})
    target_link_libraries(DCMTK::DCMTK INTERFACE ${DCMTK_LIBRARIES})
    return()
endif()

# Manual fallback. Homebrew on macOS installs DCMTK at /opt/homebrew/opt/dcmtk
# (Apple Silicon) or /usr/local/opt/dcmtk (Intel).
set(DCMTK_HINTS
    /opt/homebrew/opt/dcmtk
    /usr/local/opt/dcmtk
    /usr/local
    /usr
)

find_path(DCMTK_INCLUDE_DIR
    NAMES dcmtk/dcmnet/dimse.h
    HINTS ${DCMTK_HINTS}
    PATH_SUFFIXES include
)

# DCMTK ships many static libs that link together. We need the network +
# data dictionary + foundation libs. Order matters at link time: dependents
# before dependencies.
set(_dcmtk_libs
    dcmnet
    dcmdata
    oflog
    ofstd
)

set(DCMTK_LIBRARIES "")
foreach(_lib IN LISTS _dcmtk_libs)
    find_library(DCMTK_${_lib}_LIB
        NAMES ${_lib}
        HINTS ${DCMTK_HINTS}
        PATH_SUFFIXES lib
    )
    if(DCMTK_${_lib}_LIB)
        list(APPEND DCMTK_LIBRARIES ${DCMTK_${_lib}_LIB})
    else()
        message(WARNING "FindDCMTK: missing library: ${_lib}")
    endif()
endforeach()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DCMTK
    REQUIRED_VARS DCMTK_INCLUDE_DIR DCMTK_LIBRARIES
)

if(DCMTK_FOUND)
    add_library(DCMTK::DCMTK INTERFACE IMPORTED)
    target_include_directories(DCMTK::DCMTK INTERFACE ${DCMTK_INCLUDE_DIR})
    target_link_libraries(DCMTK::DCMTK INTERFACE ${DCMTK_LIBRARIES})

    # macOS frameworks DCMTK needs.
    if(APPLE)
        target_link_libraries(DCMTK::DCMTK INTERFACE
            "-framework CoreFoundation"
            "-framework Security"
        )
    endif()

    # zlib is a transitive dep on most builds.
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        target_link_libraries(DCMTK::DCMTK INTERFACE ZLIB::ZLIB)
    endif()
endif()
