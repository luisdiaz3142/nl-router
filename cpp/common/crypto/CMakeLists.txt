# nl_router_crypto — small static lib for AES-256-GCM envelope decryption
# of credential payloads. Used by the dispatcher; the API does its own
# encryption in Python.
#
# Depends on OpenSSL (EVP layer). DCMTK already pulls OpenSSL into our
# build transitively; this target declares it explicitly so the library
# is reusable without DCMTK.

find_package(OpenSSL REQUIRED)

add_library(nl_router_crypto
    src/envelope.cpp
    src/kek.cpp
)
add_library(nl_router::crypto ALIAS nl_router_crypto)

target_include_directories(nl_router_crypto PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(nl_router_crypto PUBLIC
    OpenSSL::Crypto
)
