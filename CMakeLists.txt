cmake_minimum_required(VERSION 3.20)
project(airplay-windows VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    add_definitions(-D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---- libplist (required) ------------------------------------------------------
# UxPlay uses libplist to serialise the /info, ANNOUNCE and other RTSP bodies.
# We take the same dependency rather than writing a bplist00 encoder by hand.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPLIST IMPORTED_TARGET libplist-2.0)
endif()
if(NOT LIBPLIST_FOUND)
    # vcpkg / CMake-config fallback (used on Windows via vcpkg manifest).
    find_package(libplist CONFIG QUIET)
endif()

# ---- OpenSSL (required) -------------------------------------------------------
# Used for Ed25519 identity keys, X25519 ECDH, AES-CTR and SHA-512 during the
# AirPlay pair-setup / pair-verify handshake.
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# ---- Sources ------------------------------------------------------------------
add_executable(airplay-windows
    src/main.cpp
    src/log.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
    src/mdns/mdns_service.cpp
    src/crypto/identity.cpp
    src/crypto/pair_verify.cpp
    src/crypto/fairplay.cpp
    src/airplay/info_plist.cpp
    src/airplay/rtsp_parser.cpp
    src/airplay/sdp.cpp
    src/airplay/server.cpp
    src/airplay/streams.cpp
    src/airplay/routes.cpp
)

target_include_directories(airplay-windows PRIVATE src)
target_link_libraries(airplay-windows PRIVATE OpenSSL::Crypto)

# ---- FairPlay SAP blobs (swap-ready) ------------------------------------------
# By default we link the zero-filled stub. If the user drops a real blob
# file into third_party/ (conventionally named fairplay_blobs_real.cpp),
# we link that instead. See docs/FAIRPLAY.md.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/fairplay_blobs_real.cpp")
    message(STATUS "FairPlay: using provisioned blob (fairplay_blobs_real.cpp)")
    target_sources(airplay-windows PRIVATE
        third_party/fairplay_blobs_real.cpp)
else()
    message(STATUS "FairPlay: using zero-filled STUB — iOS handshake will "
                   "fail at /fp-setup until a real blob is provided")
    target_sources(airplay-windows PRIVATE
        third_party/fairplay_blobs_stub.cpp)
endif()

if(LIBPLIST_FOUND)
    target_link_libraries(airplay-windows PRIVATE PkgConfig::LIBPLIST)
    target_compile_definitions(airplay-windows PRIVATE HAVE_LIBPLIST=1)
elseif(TARGET libplist::libplist-2.0)
    target_link_libraries(airplay-windows PRIVATE libplist::libplist-2.0)
    target_compile_definitions(airplay-windows PRIVATE HAVE_LIBPLIST=1)
elseif(TARGET libplist::libplist)
    target_link_libraries(airplay-windows PRIVATE libplist::libplist)
    target_compile_definitions(airplay-windows PRIVATE HAVE_LIBPLIST=1)
else()
    message(WARNING
        "libplist not found. /info will return a text placeholder. "
        "On Windows: run `vcpkg install libplist` or use the provided vcpkg.json. "
        "On Linux: `apt install libplist-dev`.")
endif()

if(WIN32)
    # ws2_32: Winsock; iphlpapi: GetAdaptersAddresses; dnsapi: native mDNS.
    target_link_libraries(airplay-windows PRIVATE ws2_32 iphlpapi dnsapi)
endif()
