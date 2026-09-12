# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/HTTP -- the libcurl-backed fetcher (zahlen_http).
#
# Its own target rather than a corner of zahlen_extras, for the same reason the
# other dependency-carrying extras have one: libcurl is not vendored, it drags a
# TLS backend in with it, and a host without it must still get the extras
# archive. ../CMakeLists.txt excludes this directory from its source globs, so
# nothing here is compiled twice and nothing here is compiled at all when
# libcurl is missing.
#
# Discovery is local to this file on purpose, the way extras/SVG keeps resvg's
# to itself -- the question "is libcurl here?" belongs to the extra that needs
# it. Public headers are included as <HTTP/HTTP.hpp>.
#
# When libcurl is not found this file warns and returns: no target, no failure.
# Consumers guard on `if(TARGET zahlen_http)`, the way they guard on
# zahlen_svg. Pass -DZHLN_BUILD_HTTP=OFF to skip the search (and the warning)
# outright.

set(TARGET_NAME zahlen_http)

option(ZHLN_BUILD_HTTP "Build extras/HTTP, the libcurl-backed HTTP fetcher" ON)

if(NOT ZHLN_BUILD_HTTP)
    return()
endif()

# --- libcurl discovery -----------------------------------------------------
# Three spellings, tried in order:
#
#   1. find_package(CURL), which on a modern CMake prefers libcurl's own
#      config package (vcpkg, a hand-installed prefix) and otherwise falls back
#      to the FindCURL module -- that is what sees a distro's libcurl4-openssl-dev
#      or Homebrew's curl, and it defines CURL::libcurl either way
#   2. pkg-config (libcurl.pc), for a prefix the module cannot see
#   3. a bare header + library pair named with -DCURL_ROOT=/path/to/prefix or
#      $CURL_ROOT
#
# None of them is REQUIRED. A miss costs a warning, not a configure error.

set(ZHLN_HTTP_TARGET "")
set(ZHLN_HTTP_ORIGIN "")
set(ZHLN_HTTP_VERSION "")
set(ZHLN_HTTP_STATIC FALSE)

find_package(CURL QUIET)

if(CURL_FOUND AND TARGET CURL::libcurl)
    set(ZHLN_HTTP_TARGET CURL::libcurl)
    set(ZHLN_HTTP_ORIGIN "find_package(CURL)")
    if(CURL_VERSION_STRING)
        set(ZHLN_HTTP_VERSION "${CURL_VERSION_STRING}")
    endif()
endif()

if(NOT ZHLN_HTTP_TARGET)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        # QUIET, not REQUIRED: extras/json's simdjson check is REQUIRED because
        # the extras archive cannot build without it, and this one is not.
        pkg_check_modules(ZHLN_LIBCURL QUIET IMPORTED_TARGET libcurl)
        if(ZHLN_LIBCURL_FOUND AND TARGET PkgConfig::ZHLN_LIBCURL)
            set(ZHLN_HTTP_TARGET PkgConfig::ZHLN_LIBCURL)
            set(ZHLN_HTTP_ORIGIN "pkg-config libcurl")
            if(ZHLN_LIBCURL_VERSION)
                set(ZHLN_HTTP_VERSION "${ZHLN_LIBCURL_VERSION}")
            endif()
        endif()
    endif()
endif()

# Neither name is initialised here, and that is deliberate: find_path and
# find_library write CACHE entries, and a normal variable of the same name set
# beforehand would shadow the result -- the classic way a library that was found
# reads back as empty.
if(NOT ZHLN_HTTP_TARGET)
    find_path(ZHLN_HTTP_BARE_INCLUDE_DIR
        NAMES curl/curl.h
        HINTS "${CURL_ROOT}" ENV CURL_ROOT
        PATH_SUFFIXES include
    )
    find_library(ZHLN_HTTP_BARE_LIBRARY
        NAMES curl libcurl libcurl_imp
        HINTS "${CURL_ROOT}" ENV CURL_ROOT
        PATH_SUFFIXES lib lib64
    )

    if(ZHLN_HTTP_BARE_INCLUDE_DIR AND ZHLN_HTTP_BARE_LIBRARY)
        add_library(ZHLN::curl INTERFACE IMPORTED)
        set_target_properties(ZHLN::curl PROPERTIES
            INTERFACE_LINK_LIBRARIES "${ZHLN_HTTP_BARE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZHLN_HTTP_BARE_INCLUDE_DIR}"
        )
        set(ZHLN_HTTP_TARGET ZHLN::curl)
        set(ZHLN_HTTP_ORIGIN "${ZHLN_HTTP_BARE_LIBRARY}")
    endif()
endif()

if(NOT ZHLN_HTTP_TARGET)
    message(WARNING
        "extras/HTTP: libcurl was not found, so zahlen_http will not be built "
        "and <HTTP/HTTP.hpp> will not exist for consumers. Install libcurl with "
        "its development headers (Debian and Ubuntu: `libcurl4-openssl-dev`; "
        "Fedora: `libcurl-devel`; Arch: `curl`; Homebrew: `curl`; vcpkg: "
        "`curl`), or point CMake at an existing prefix with "
        "-DCURL_ROOT=/path/to/prefix. Everything else in extras/ is unaffected. "
        "Pass -DZHLN_BUILD_HTTP=OFF to silence this.")
    return()
endif()

# --- version floor ---------------------------------------------------------
# HTTP.cpp is written against libcurl 7.69 and says so with an #error of its
# own; reading curlver.h costs nothing and turns a wall of compiler errors into
# one line here. 7.69 is where code 8 stopped being FTP-specific and got the
# CURLE_WEIRD_SERVER_REPLY name MapCurlError uses.
#
# Best effort: a target that hides its include directory leaves the version
# unknown, and an unknown version is allowed through for the compiler to judge.
set(ZHLN_HTTP_MINIMUM_VERSION "7.69")

set(ZHLN_HTTP_HEADER_HINTS "${CURL_ROOT}")
get_target_property(ZHLN_HTTP_TARGET_INCLUDE_DIRS ${ZHLN_HTTP_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
if(ZHLN_HTTP_TARGET_INCLUDE_DIRS)
    list(APPEND ZHLN_HTTP_HEADER_HINTS ${ZHLN_HTTP_TARGET_INCLUDE_DIRS})
endif()
if(ZHLN_LIBCURL_INCLUDE_DIRS)
    list(APPEND ZHLN_HTTP_HEADER_HINTS ${ZHLN_LIBCURL_INCLUDE_DIRS})
endif()

find_path(ZHLN_HTTP_HEADER_DIR
    NAMES curl/curlver.h
    HINTS ${ZHLN_HTTP_HEADER_HINTS} ENV CURL_ROOT
    PATH_SUFFIXES include
)

set(ZHLN_HTTP_CURLVER "")
foreach(ZHLN_HTTP_CANDIDATE_DIR IN LISTS ZHLN_HTTP_HEADER_DIR)
    if(EXISTS "${ZHLN_HTTP_CANDIDATE_DIR}/curl/curlver.h")
        set(ZHLN_HTTP_CURLVER "${ZHLN_HTTP_CANDIDATE_DIR}/curl/curlver.h")
        break()
    endif()
endforeach()

if(ZHLN_HTTP_CURLVER)
    file(STRINGS "${ZHLN_HTTP_CURLVER}" ZHLN_HTTP_VERSION_LINES REGEX "^#define LIBCURL_VERSION ")
    foreach(ZHLN_HTTP_LINE IN LISTS ZHLN_HTTP_VERSION_LINES)
        # Only the numeric part: a master checkout writes "8.16.0-DEV", and a
        # suffix would take the VERSION_LESS below out of its depth.
        if(ZHLN_HTTP_LINE MATCHES "^#define LIBCURL_VERSION[ \t]+\"([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
            set(ZHLN_HTTP_VERSION "${CMAKE_MATCH_1}")
            break()
        endif()
    endforeach()
endif()

if(NOT ZHLN_HTTP_VERSION STREQUAL "" AND ZHLN_HTTP_VERSION VERSION_LESS ZHLN_HTTP_MINIMUM_VERSION)
    message(WARNING
        "extras/HTTP: found libcurl ${ZHLN_HTTP_VERSION} (${ZHLN_HTTP_ORIGIN}), "
        "but ${ZHLN_HTTP_MINIMUM_VERSION} is the oldest release HTTP.cpp is "
        "written against, so zahlen_http will not be built. Upgrade libcurl, or "
        "pass -DZHLN_BUILD_HTTP=OFF to silence this.")
    return()
endif()

# --- target ----------------------------------------------------------------

add_library(${TARGET_NAME} STATIC
    HTTP.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: HTTP.hpp includes <Zahlen/Error.hpp> and every
# signature in it returns std::expected<T, ZHLN::Error>, so a consumer needs
# core's include path and its error-category registration, not just this
# archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# libcurl stays PRIVATE, the way resvg does in extras/SVG and simdjson does in
# ../CMakeLists.txt: HTTP.hpp names no curl type and includes no curl header, so
# neither the include path nor the library can leak into a consumer, and a
# libcurl upgrade recompiles this one file.
target_link_libraries(${TARGET_NAME} PRIVATE
    ${ZHLN_HTTP_TARGET}
)

# A static libcurl -- the .a of a bare prefix, or a Windows libcurl.lib -- needs
# two things a shared one has already settled inside itself: the CURL_STATICLIB
# define, without which curl.h spells every entry point __declspec(dllimport)
# and MSVC fails the link, and the threading library.
get_target_property(ZHLN_HTTP_LINKED ${ZHLN_HTTP_TARGET} INTERFACE_LINK_LIBRARIES)
if(ZHLN_HTTP_BARE_LIBRARY AND NOT ZHLN_HTTP_BARE_LIBRARY MATCHES "\\.so|\\.dylib|\\.dll")
    set(ZHLN_HTTP_STATIC TRUE)
elseif(ZHLN_HTTP_LINKED)
    foreach(ZHLN_HTTP_LINKED_ENTRY IN LISTS ZHLN_HTTP_LINKED)
        if(ZHLN_HTTP_LINKED_ENTRY MATCHES "libcurl\\.a$|libcurl\\.lib$|curl\\.lib$")
            set(ZHLN_HTTP_STATIC TRUE)
        endif()
    endforeach()
endif()

if(ZHLN_HTTP_STATIC)
    target_compile_definitions(${TARGET_NAME} PRIVATE CURL_STATICLIB)
    find_package(Threads QUIET)
    if(TARGET Threads::Threads)
        target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
    elseif(UNIX)
        target_link_libraries(${TARGET_NAME} PRIVATE pthread)
    endif()
    if(UNIX)
        target_link_libraries(${TARGET_NAME} PRIVATE
            ${CMAKE_DL_LIBS}
            m
        )
    endif()
endif()

# Public headers are included as <HTTP/HTTP.hpp>, so the extras root is the only
# include path this target publishes.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# Nothing links zahlen_http automatically: unlike the editor and the scripting
# runtime, no part of the composition root consumes it yet, and an unused static
# library is still a libcurl dependency at link time for whoever inherits it. A
# host that wants fetching links zahlen_http itself, guarded on TARGET.
if(ZHLN_HTTP_VERSION STREQUAL "")
    message(STATUS "extras/HTTP: building zahlen_http against libcurl (${ZHLN_HTTP_ORIGIN}), version not reported.")
else()
    message(STATUS "extras/HTTP: building zahlen_http against libcurl ${ZHLN_HTTP_VERSION} (${ZHLN_HTTP_ORIGIN}).")
endif()
