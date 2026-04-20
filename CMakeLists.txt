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

# ---- Bonjour SDK (Windows only, required for mDNS) ----------------------------
if(WIN32)
    if(NOT DEFINED ENV{BONJOUR_SDK_HOME})
        set(BONJOUR_SDK_HOME "C:/Program Files/Bonjour SDK")
    else()
        set(BONJOUR_SDK_HOME "$ENV{BONJOUR_SDK_HOME}")
    endif()

    find_path(BONJOUR_INCLUDE_DIR dns_sd.h
        HINTS "${BONJOUR_SDK_HOME}/Include")
    find_library(BONJOUR_LIBRARY NAMES dnssd
        HINTS "${BONJOUR_SDK_HOME}/Lib/x64" "${BONJOUR_SDK_HOME}/Lib/Win32")

    if(NOT BONJOUR_INCLUDE_DIR OR NOT BONJOUR_LIBRARY)
        message(WARNING
            "Bonjour SDK not found. Install from https://developer.apple.com/bonjour/ "
            "and set BONJOUR_SDK_HOME. The project will build without mDNS support "
            "(discovery will not work).")
        set(HAS_BONJOUR OFF)
    else()
        set(HAS_BONJOUR ON)
    endif()
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

# ---- Sources ------------------------------------------------------------------
add_executable(airplay-windows
    src/main.cpp
    src/log.cpp
    src/net/socket.cpp
    src/net/tcp_server.cpp
    src/mdns/bonjour_service.cpp
    src/airplay/info_plist.cpp
    src/airplay/rtsp_parser.cpp
    src/airplay/server.cpp
    src/airplay/routes.cpp
)

target_include_directories(airplay-windows PRIVATE src)

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
    target_link_libraries(airplay-windows PRIVATE ws2_32 iphlpapi)
    if(HAS_BONJOUR)
        target_include_directories(airplay-windows PRIVATE "${BONJOUR_INCLUDE_DIR}")
        target_link_libraries(airplay-windows PRIVATE "${BONJOUR_LIBRARY}")
        target_compile_definitions(airplay-windows PRIVATE HAVE_BONJOUR=1)
    endif()
endif()
