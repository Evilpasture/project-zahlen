# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/SlangPrebuilt.cmake
#
# libslang without a Slang build.
#
# The official release archive is a complete Slang install: lib/libslang-compiler,
# the slang-glslang companion, the public headers (slang.h, slang-com-helper.h,
# slang-com-ptr.h) and a CMake config package at lib/cmake/slang. The archive
# also ships a standalone compiler binary, which this build ignores: zshader
# links libslang once and is the only shader compiler driver. SlangResolve.cmake
# consumes the library/header layout -- the same one
# scripts/install-slang-prebuilt.sh produces -- and nothing else.
#
# Why the release archive and not the PyPI slangpy wheel: the wheel carries
# libslang-compiler and slang.h, but neither slang-com-helper.h nor
# slang-com-ptr.h (verified against slangpy 0.43.1: 134 headers, no such files),
# and utils/zshader includes both. It also has no CMake config, so the library
# would have to be re-described by hand. The archive has all of it.
#
# Ordering, highest first:
#
#   1. an existing Slang in the configure (a parent project, an earlier add)
#   2. a host slang CONFIG package / slangpy wheel (libslang + headers)
#   3. this file: the release archive, unpacked into the build tree and cached
#   4. extern/slang compiled from source -- ZHLN_SLANG_VENDORED=ON only
#
# Every switch here is a cache entry, so `cmake -S . -B build/x && cmake --build
# build/x` is the whole story. scripts/build.sh only names the switches.
#
# Nothing here touches a compiler's C++ runtime flags, and nothing from the
# archive is ever compiled: it is a prebuilt SDK, linked against its publisher's
# runtime, which on every platform Slang ships for is the platform's own.

include_guard(GLOBAL)

# Archive fallback is Linux-only; source selection belongs to SlangResolve.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ZHLN_SLANG_VENDORED)
    return()
endif()

option(ZHLN_SLANG_FROM_PREBUILT
    "Download the official Slang release archive when no host Slang is usable" ON)
set(ZHLN_SLANG_RELEASE "" CACHE STRING
    "Slang release tag to fetch (empty asks the GitHub API for the latest)")
set(ZHLN_SLANG_PREBUILT_ROOT "" CACHE PATH
    "Use an already-unpacked Slang release as-is; skips the download")
set(ZHLN_SLANG_PREBUILT_DIR "" CACHE PATH "Resolved Slang release root (set by this file)")
set(ZHLN_SLANG_KNOWN_GOOD "v2026.21" CACHE STRING
    "Release used when the GitHub API cannot be reached")

# The release names its assets <tag>-<os>-<arch>. Those three tokens are enough to
# name the archive without asking the API -- which is what makes a pinned,
# offline, reproducible fetch possible at all.
# Every dead end below goes through this, so the message is the same shape and the
# requirement switch is honoured in one place.
function(_zhln_slang_give_up _reason)
    if(ZHLN_SLANG_REQUIRE_PREBUILT)
        message(FATAL_ERROR
            "Slang: ${_reason}. ZHLN_SLANG_REQUIRE_PREBUILT=ON, so this is fatal rather than a "
            "fallback. Fix the cause, pin a release with -DZHLN_SLANG_RELEASE=vYYYY.MM, point "
            "-DZHLN_SLANG_PREBUILT_ROOT at an unpacked release, or turn the requirement off and "
            "accept a host libslang package / extern/slang build.")
    endif()
    message(STATUS "libslang: ${_reason}; falling back to a host package, then extern/slang")
endfunction()

function(_zhln_slang_prebuilt_asset _tag _out)
    set(_ver "${_tag}")
    if(_ver MATCHES "^v")
        string(SUBSTRING "${_ver}" 1 -1 _ver)
    endif()

    if(APPLE)
        set(_os "macos")
    elseif(WIN32)
        set(_os "windows")
    else()
        set(_os "linux")
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(_arch "aarch64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_arch "x86_64")
    else()
        set(_arch "")
    endif()

    if(NOT _arch)
        set(${_out} "" PARENT_SCOPE)
        return()
    endif()
    set(${_out} "slang-${_ver}-${_os}-${_arch}.zip" PARENT_SCOPE)
endfunction()

if(NOT ZHLN_SLANG_FROM_PREBUILT)
    return()
endif()

# Downloading is the default, but a build machine behind a proxy or without the
# GitHub API needs a way out that is not "silently compile 950 TUs". With
# ZHLN_SLANG_REQUIRE_PREBUILT=ON a failed resolve is a configure error naming the
# cause, instead of a fallthrough nobody notices until the linker runs.
option(ZHLN_SLANG_REQUIRE_PREBUILT
    "Fail the configure when the Slang release cannot be resolved, instead of falling back" OFF)

# --- an already-unpacked tree wins over downloading -------------------------
if(ZHLN_SLANG_PREBUILT_ROOT AND EXISTS "${ZHLN_SLANG_PREBUILT_ROOT}/include/slang.h")
    set(ZHLN_SLANG_PREBUILT_DIR "${ZHLN_SLANG_PREBUILT_ROOT}" CACHE PATH "" FORCE)
    message(STATUS "Slang: using the unpacked release at ${ZHLN_SLANG_PREBUILT_DIR}")
    return()
endif()

# --- an earlier configure of this tree already resolved it -----------------
if(ZHLN_SLANG_PREBUILT_DIR AND EXISTS "${ZHLN_SLANG_PREBUILT_DIR}/include/slang.h")
    message(STATUS "Slang: using the cached release at ${ZHLN_SLANG_PREBUILT_DIR}")
    return()
endif()

# --- resolve the tag --------------------------------------------------------
#
# Deliberately CMake, not Python. A `python3` that cannot verify TLS (a framework
# install without "Install Certificates.command" is the usual case) fails inside
# ssl.do_handshake and buries the configure in a traceback. file(DOWNLOAD) uses
# CMake's own libcurl and CA store, so it works wherever the compiler's toolchain
# was able to download anything at all.
set(_zhln_tag "${ZHLN_SLANG_RELEASE}")
if(NOT _zhln_tag)
    # The API answers JSON; only the tag_name field is needed, and this regex is
    # deliberately forgiving about key order and whitespace.
    file(DOWNLOAD "https://api.github.com/repos/shader-slang/slang/releases/latest"
        "${CMAKE_BINARY_DIR}/_deps/slang-release/latest.json"
        HTTPHEADER "User-Agent: zahlen-cmake"
        TLS_VERIFY ON
        STATUS _zhln_api_status)
    list(GET _zhln_api_status 0 _zhln_api_rc)
    if(_zhln_api_rc EQUAL 0
       AND EXISTS "${CMAKE_BINARY_DIR}/_deps/slang-release/latest.json")
        file(READ "${CMAKE_BINARY_DIR}/_deps/slang-release/latest.json" _zhln_api_json)
        if(_zhln_api_json MATCHES "\"tag_name\"[ \t]*:[ \t]*\"([^\"]+)\"")
            set(_zhln_tag "${CMAKE_MATCH_1}")
            message(STATUS "Slang: latest release is ${_zhln_tag} (from the GitHub API)")
        endif()
    else()
        list(GET _zhln_api_status 1 _zhln_api_msg)
        message(STATUS "Slang: release discovery failed (${_zhln_api_msg})")
    endif()
endif()

if(NOT _zhln_tag)
    # Nothing discovered and nothing pinned. Fall back to the newest tag this
    # project is known to have been built against, so an offline or rate-limited
    # machine still gets a Slang instead of a compile. Overridable by hand.
    set(_zhln_tag "${ZHLN_SLANG_KNOWN_GOOD}")
    message(STATUS
        "Slang: no release could be discovered; using the known-good ${_zhln_tag}. "
        "Pin another with -DZHLN_SLANG_RELEASE=vYYYY.MM")
endif()

_zhln_slang_prebuilt_asset("${_zhln_tag}" _zhln_asset)
if(NOT _zhln_asset)
    _zhln_slang_give_up("no release asset matches ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
    return()
endif()

set(_zhln_url "https://github.com/shader-slang/slang/releases/download/${_zhln_tag}/${_zhln_asset}")
set(_zhln_dir "${CMAKE_BINARY_DIR}/_deps/slang-release")
set(_zhln_archive "${_zhln_dir}/${_zhln_asset}")
set(_zhln_root "${_zhln_dir}/${_zhln_tag}")

file(MAKE_DIRECTORY "${_zhln_dir}")

if(NOT EXISTS "${_zhln_archive}")
    message(STATUS "Slang: fetching ${_zhln_asset} (${_zhln_tag})")
    file(DOWNLOAD "${_zhln_url}" "${_zhln_archive}"
        TLS_VERIFY ON
        STATUS _zhln_dl_status
        SHOW_PROGRESS)
    list(GET _zhln_dl_status 0 _zhln_dl_rc)
    if(NOT _zhln_dl_rc EQUAL 0)
        list(GET _zhln_dl_status 1 _zhln_dl_msg)
        file(REMOVE "${_zhln_archive}")
        _zhln_slang_give_up("downloading ${_zhln_asset} failed (${_zhln_dl_msg})")
        return()
    endif()
else()
    message(STATUS "Slang: reusing the downloaded ${_zhln_asset}")
endif()

# A truncated or HTML "not found" body would otherwise be reported as a broken
# archive by file(ARCHIVE_EXTRACT) with no clue as to the cause.
file(SIZE "${_zhln_archive}" _zhln_size)
if(_zhln_size LESS 1000000)
    _zhln_slang_give_up("${_zhln_archive} is only ${_zhln_size} bytes, so it is not a release archive")
    file(REMOVE "${_zhln_archive}")
    return()
endif()

if(NOT EXISTS "${_zhln_root}/include/slang.h")
    message(STATUS "Slang: unpacking ${_zhln_asset}")
    file(REMOVE_RECURSE "${_zhln_root}")
    file(MAKE_DIRECTORY "${_zhln_root}")
    file(ARCHIVE_EXTRACT INPUT "${_zhln_archive}" DESTINATION "${_zhln_root}")
    # ARCHIVE_EXTRACT reports nothing and there is no RESULT argument, so a
    # half-written tree (a full disk is the usual cause) would sail past this and
    # fail later as "no include/slang.h" with no cause attached. Probe here, drop
    # the partial tree so the next configure retries, and keep the archive: the
    # download was fine, the unpacking was not.
    set(_zhln_extracted FALSE)
    foreach(_zhln_after IN ITEMS "${_zhln_root}" "${_zhln_root}/slang" "${_zhln_root}/${_zhln_tag}")
        if(EXISTS "${_zhln_after}/include/slang.h")
            set(_zhln_extracted TRUE)
            break()
        endif()
    endforeach()
    if(NOT _zhln_extracted)
        file(REMOVE_RECURSE "${_zhln_root}")
        _zhln_slang_give_up(
            "${_zhln_asset} unpacked to ${_zhln_root} but produced no include/slang.h (a full disk "
            "during extraction is the usual cause; the archive was kept at ${_zhln_archive})")
        return()
    endif()
endif()

# The archive's entries are "./include/..." etc., so the root it unpacks into may
# be the destination itself or a single directory beneath it.
set(_zhln_resolved "")
foreach(_zhln_probe IN ITEMS "${_zhln_root}" "${_zhln_root}/slang" "${_zhln_root}/${_zhln_tag}")
    if(EXISTS "${_zhln_probe}/include/slang.h")
        set(_zhln_resolved "${_zhln_probe}")
        break()
    endif()
endforeach()

if(NOT _zhln_resolved)
    _zhln_slang_give_up("${_zhln_asset} unpacked to ${_zhln_root}, but no include/slang.h is under it")
    return()
endif()

set(ZHLN_SLANG_PREBUILT_DIR "${_zhln_resolved}" CACHE PATH "" FORCE)
message(STATUS "Slang: release ${_zhln_tag} unpacked at ${ZHLN_SLANG_PREBUILT_DIR}")
