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
# Try three lookup paths in order (vcpkg / Linux pkg-config / raw find_*).
set(LIBPLIST_TARGET "")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBPLIST IMPORTED_TARGET libplist-2.0 QUIET)
    if(LIBPLIST_FOUND)
        set(LIBPLIST_TARGET PkgConfig::LIBPLIST)
    endif()
endif()

if(NOT LIBPLIST_TARGET)
    find_package(libplist CONFIG QUIET)
    foreach(_t libplist::libplist-2.0 libplist::libplist unofficial::libplist::libplist)
        if(TARGET ${_t})
            set(LIBPLIST_TARGET ${_t})
            break()
        endif()
    endforeach()
endif()

if(NOT LIBPLIST_TARGET)
    # vcpkg doesn't ship a CMake config for libplist on all platforms — fall
    # back to raw finders. With the vcpkg toolchain, CMAKE_PREFIX_PATH already
    # points at vcpkg_installed/<triplet>/{include,lib}, so these just work.
    find_path(LIBPLIST_INCLUDE_DIR plist/plist.h)
    find_library(LIBPLIST_LIBRARY NAMES plist-2.0 libplist-2.0 plist libplist)
    if(LIBPLIST_INCLUDE_DIR AND LIBPLIST_LIBRARY)
        message(STATUS "libplist: raw find — include=${LIBPLIST_INCLUDE_DIR} "
                       "lib=${LIBPLIST_LIBRARY}")
        add_library(airplay_libplist INTERFACE)
        target_include_directories(airplay_libplist INTERFACE "${LIBPLIST_INCLUDE_DIR}")
        target_link_libraries(airplay_libplist INTERFACE "${LIBPLIST_LIBRARY}")
        set(LIBPLIST_TARGET airplay_libplist)
    endif()
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
    src/airplay/airplay2_setup.cpp
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

if(LIBPLIST_TARGET)
    message(STATUS "libplist: linking via ${LIBPLIST_TARGET}")
    target_link_libraries(airplay-windows PRIVATE ${LIBPLIST_TARGET})
    target_compile_definitions(airplay-windows PRIVATE HAVE_LIBPLIST=1)
else()
    message(FATAL_ERROR
        "libplist not found. Required for the /info binary plist — iOS will "
        "reject the handshake without it.\n"
        "  Windows: re-run cmake with -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/"
        "buildsystems/vcpkg.cmake (vcpkg.json declares the dep).\n"
        "  Linux:   apt install libplist-dev")
endif()

if(WIN32)
    # ws2_32: Winsock; iphlpapi: GetAdaptersAddresses; dnsapi: native mDNS.
    target_link_libraries(airplay-windows PRIVATE ws2_32 iphlpapi dnsapi)
endif()
