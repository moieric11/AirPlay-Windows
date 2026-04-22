cmake_minimum_required(VERSION 3.20)
project(airplay-windows VERSION 0.1.0 LANGUAGES C CXX)

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

# ---- FFmpeg (required for H.264 decoding) -------------------------------------
# Uses libavcodec to decode the clear H.264 NAL units the mirror stream
# produces after AES-CTR decrypt + AVCC→Annex-B conversion. libswscale is
# for YUV→RGB conversion when dumping debug frames to PPM.
set(FFMPEG_TARGETS "")
if(PkgConfig_FOUND)
    pkg_check_modules(AVCODEC  IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL   IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE  IMPORTED_TARGET libswscale)
    if(AVCODEC_FOUND AND AVUTIL_FOUND AND SWSCALE_FOUND)
        set(FFMPEG_TARGETS PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWSCALE)
    endif()
endif()
if(NOT FFMPEG_TARGETS)
    # vcpkg path (Windows): use its FindFFMPEG module.
    find_package(FFMPEG QUIET)
    if(FFMPEG_FOUND)
        set(FFMPEG_TARGETS ${FFMPEG_LIBRARIES})
        set(FFMPEG_INCLUDE_DIRS_OK "${FFMPEG_INCLUDE_DIRS}")
    endif()
endif()
if(NOT FFMPEG_TARGETS)
    message(FATAL_ERROR
        "FFmpeg (libavcodec + libavutil + libswscale) not found.\n"
        "  Windows: vcpkg.json already declares the dependency — rerun cmake "
        "with the vcpkg toolchain.\n"
        "  Linux:   apt install libavcodec-dev libswscale-dev")
endif()

# ---- SDL2 (required for the video renderer window) ----------------------------
# Used by video/VideoRenderer to create a real-time window displaying the
# decoded iPhone screen. pkg-config on Linux, SDL2 CMake config on Windows
# (installed by vcpkg).
set(SDL2_TARGET "")
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
    if(SDL2_FOUND)
        set(SDL2_TARGET PkgConfig::SDL2)
    endif()
endif()
if(NOT SDL2_TARGET)
    find_package(SDL2 CONFIG QUIET)
    if(TARGET SDL2::SDL2)
        set(SDL2_TARGET SDL2::SDL2)
    elseif(TARGET SDL2::SDL2-static)
        set(SDL2_TARGET SDL2::SDL2-static)
    endif()
endif()
if(NOT SDL2_TARGET)
    message(FATAL_ERROR
        "SDL2 not found.\n"
        "  Windows: rerun cmake with the vcpkg toolchain.\n"
        "  Linux:   apt install libsdl2-dev")
endif()

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
    src/crypto/mirror_decrypt.cpp
    src/airplay/airplay2_setup.cpp
    src/airplay/daap.cpp
    src/airplay/h264_nal.cpp
    src/airplay/h264_sps.cpp
    src/airplay/info_plist.cpp
    src/airplay/mirror_listener.cpp
    src/airplay/ntp_client.cpp
    src/airplay/rtsp_parser.cpp
    src/airplay/sdp.cpp
    src/airplay/server.cpp
    src/airplay/streams.cpp
    src/airplay/routes.cpp
    src/video/h264_decoder.cpp
    src/video/video_renderer.cpp
    src/audio/audio_receiver.cpp
    src/audio/aac_decoder.cpp
    src/audio/audio_output.cpp
)

target_include_directories(airplay-windows PRIVATE src)
target_link_libraries(airplay-windows PRIVATE OpenSSL::Crypto ${FFMPEG_TARGETS} ${SDL2_TARGET})
if(DEFINED FFMPEG_INCLUDE_DIRS_OK)
    target_include_directories(airplay-windows PRIVATE ${FFMPEG_INCLUDE_DIRS_OK})
endif()

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

# ---- playfair (level-B FairPlay decrypt, swap-ready) --------------------------
# If third_party/playfair/ exists (copied from UxPlay's lib/playfair/), compile
# and link its C files so fairplay_decrypt(ekey) can recover the AES stream key.
# Without it, decryption is disabled and we only log the encrypted NAL units.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/playfair/playfair.c")
    message(STATUS "playfair: using provisioned decryption library")
    set(_playfair_srcs
        third_party/playfair/playfair.c
        third_party/playfair/hand_garble.c
        third_party/playfair/omg_hax.c
        third_party/playfair/modified_md5.c
        third_party/playfair/sap_hash.c)
    target_sources(airplay-windows PRIVATE ${_playfair_srcs})
    # UxPlay's source uses `#include "playfair/playfair.h"` from fairplay_playfair.c
    # and `#include "playfair.h"` from within playfair/*.c, so we expose both.
    target_include_directories(airplay-windows PRIVATE
        third_party
        third_party/playfair)
    target_compile_definitions(airplay-windows PRIVATE HAVE_PLAYFAIR=1)

    # Quiet down playfair's legacy-C warnings (intentional byte truncations,
    # unused-variable debug leftovers, a spurious C4701 in modified_md5.c).
    # We keep our own source compiled at /W4 to catch real issues.
    if(MSVC)
        set_source_files_properties(${_playfair_srcs} PROPERTIES
            COMPILE_FLAGS "/wd4100 /wd4101 /wd4189 /wd4244 /wd4305 /wd4701")
    else()
        set_source_files_properties(${_playfair_srcs} PROPERTIES
            COMPILE_FLAGS "-w")
    endif()
else()
    message(STATUS "playfair: not provisioned — fairplay_decrypt() will be a no-op")
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
