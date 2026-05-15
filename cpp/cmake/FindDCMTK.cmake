# Minimal DCMTK locator.
#
# Tries DCMTK's own CMake config first (works for system installs that ship a
# DCMTKConfig.cmake), then falls back to a manual search via headers + lib
# names. The fallback works on Homebrew installs where DCMTK's bundled CMake
# package config can lag behind library locations.
#
# After this module runs, the imported target `DCMTK::DCMTK` is available.
# Pass it to target_link_libraries() and it picks up include dirs and link
# libs automatically.

if(TARGET DCMTK::DCMTK)
    return()
endif()

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
