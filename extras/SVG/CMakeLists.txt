# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# extras/SVG -- the resvg-backed SVG rasterizer (zahlen_svg).
#
# Its own target rather than a corner of zahlen_extras, for the same reason the
# other dependency-carrying extras have one: resvg is a Rust library, it is not
# vendored, and a host without it must still get the extras archive. ../CMakeLists.txt
# excludes this directory from its source globs, so nothing here is compiled
# twice and nothing here is compiled at all when resvg is missing.
#
# Discovery is local to this file on purpose -- there is no cmake/Findresvg.cmake
# and no entry in the root CMakeLists -- because the question "is resvg here?"
# belongs to the extra that needs it. Public headers are included as
# <SVG/SVG.hpp>.
#
# When resvg is not found this file warns and returns: no target, no failure.
# Consumers guard on `if(TARGET zahlen_svg)`, the way the composition root
# guards on zahlen_editor. Pass -DZHLN_BUILD_SVG=OFF to skip the search (and the
# warning) outright.

set(TARGET_NAME zahlen_svg)

option(ZHLN_BUILD_SVG "Build extras/SVG, the resvg-backed SVG rasterizer" ON)

if(NOT ZHLN_BUILD_SVG)
    return()
endif()

# --- resvg discovery -------------------------------------------------------
# resvg is Rust, and how it reached this machine decides how CMake can see it.
# Three spellings, tried in order:
#
#   1. a CMake package defining resvg::resvg, for a vendored or hand-packaged
#      build exported with install(EXPORT)
#   2. pkg-config (resvg.pc), which is what cargo-c writes and therefore what
#      the distro packages ship -- Arch's `resvg` and Homebrew's `resvg` both
#      install lib/pkgconfig/resvg.pc beside include/resvg/resvg.h
#   3. a bare header + library pair under a prefix that ships no metadata at
#      all, named with -DRESVG_ROOT=/path/to/prefix or $RESVG_ROOT
#
# None of them is REQUIRED. A miss costs a warning, not a configure error.

set(ZHLN_SVG_TARGET "")
set(ZHLN_SVG_ORIGIN "")
set(ZHLN_SVG_VERSION "")

find_package(resvg CONFIG QUIET)

# A hand-written config file gets to choose its own target name, so take the
# first of the usual spellings that exists.
foreach(ZHLN_SVG_PACKAGE_TARGET resvg::resvg resvg::capi unofficial-resvg::resvg)
    if(TARGET ${ZHLN_SVG_PACKAGE_TARGET})
        set(ZHLN_SVG_TARGET ${ZHLN_SVG_PACKAGE_TARGET})
        set(ZHLN_SVG_ORIGIN "CMake package ${ZHLN_SVG_PACKAGE_TARGET}")
        if(DEFINED resvg_VERSION)
            set(ZHLN_SVG_VERSION "${resvg_VERSION}")
        endif()
        break()
    endif()
endforeach()

if(NOT ZHLN_SVG_TARGET)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        # QUIET, not REQUIRED: extras/json's simdjson check is REQUIRED because
        # the extras archive cannot build without it, and this one is not.
        pkg_check_modules(ZHLN_RESVG QUIET IMPORTED_TARGET resvg)
        if(ZHLN_RESVG_FOUND AND TARGET PkgConfig::ZHLN_RESVG)
            set(ZHLN_SVG_TARGET PkgConfig::ZHLN_RESVG)
            set(ZHLN_SVG_ORIGIN "pkg-config resvg")
            if(ZHLN_RESVG_VERSION)
                set(ZHLN_SVG_VERSION "${ZHLN_RESVG_VERSION}")
            endif()
        endif()
    endif()
endif()

# Neither name is initialised here, and that is deliberate: find_path and
# find_library write CACHE entries, and a normal variable of the same name set
# beforehand would shadow the result -- the classic way a library that was found
# reads back as empty. An undefined ZHLN_SVG_BARE_LIBRARY below expands to ""
# and its MATCHES is simply false. The bare library is also what decides whether
# a static libresvg.a needs the Rust runtime's own dependencies added by hand.
if(NOT ZHLN_SVG_TARGET)
    find_path(ZHLN_SVG_BARE_INCLUDE_DIR
        NAMES resvg/resvg.h resvg.h
        HINTS "${RESVG_ROOT}" ENV RESVG_ROOT
        PATH_SUFFIXES include
    )
    find_library(ZHLN_SVG_BARE_LIBRARY
        NAMES resvg
        HINTS "${RESVG_ROOT}" ENV RESVG_ROOT
        PATH_SUFFIXES lib lib64
    )

    if(ZHLN_SVG_BARE_INCLUDE_DIR AND ZHLN_SVG_BARE_LIBRARY)
        add_library(ZHLN::resvg INTERFACE IMPORTED)
        set_target_properties(ZHLN::resvg PROPERTIES
            INTERFACE_LINK_LIBRARIES "${ZHLN_SVG_BARE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ZHLN_SVG_BARE_INCLUDE_DIR}"
        )
        set(ZHLN_SVG_TARGET ZHLN::resvg)
        set(ZHLN_SVG_ORIGIN "${ZHLN_SVG_BARE_LIBRARY}")
    endif()
endif()

if(NOT ZHLN_SVG_TARGET)
    message(WARNING
        "extras/SVG: resvg was not found, so zahlen_svg will not be built and "
        "<SVG/SVG.hpp> will not exist for consumers. Install the resvg C API "
        "(Arch and Homebrew: `resvg`; elsewhere build "
        "https://github.com/linebender/resvg with cargo-c, which installs "
        "resvg.h, libresvg and resvg.pc together), or point CMake at an "
        "existing prefix with -DRESVG_ROOT=/path/to/prefix. Everything else in "
        "extras/ is unaffected. Pass -DZHLN_BUILD_SVG=OFF to silence this.")
    return()
endif()

# --- version floor ---------------------------------------------------------
# SVG.cpp is written against the C API resvg has shipped since 0.42 (unchanged
# through 0.48): resvg_render takes a transform plus an explicit pixel size, and
# resvg_size/resvg_rect are float members. Older releases also pass a
# resvg_fit_to, so there is no spelling of "render into exactly these pixels" to
# call. Reading the header costs nothing and turns a wall of compiler errors
# into one line here.
#
# Best effort: a package that hides its include directory (pkg-config drops
# -I/usr/include as a system path) leaves the version unknown, and an unknown
# version is allowed through for the compiler to judge.
set(ZHLN_SVG_MINIMUM_VERSION "0.42")

# Wherever the library came from, its header may be somewhere else again: a
# pkg-config module that reports no -I (pkg-config drops the default system
# include path) still installs /usr/include/resvg/resvg.h, and a CMake package
# reports its include directory as a target property rather than a variable.
set(ZHLN_SVG_HEADER_HINTS "${RESVG_ROOT}")
get_target_property(ZHLN_SVG_TARGET_INCLUDE_DIRS ${ZHLN_SVG_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
if(ZHLN_SVG_TARGET_INCLUDE_DIRS)
    list(APPEND ZHLN_SVG_HEADER_HINTS ${ZHLN_SVG_TARGET_INCLUDE_DIRS})
endif()
if(ZHLN_RESVG_INCLUDE_DIRS)
    list(APPEND ZHLN_SVG_HEADER_HINTS ${ZHLN_RESVG_INCLUDE_DIRS})
endif()

find_path(ZHLN_SVG_HEADER_DIR
    NAMES resvg/resvg.h resvg.h
    HINTS ${ZHLN_SVG_HEADER_HINTS} ENV RESVG_ROOT
    PATH_SUFFIXES include
)

set(ZHLN_SVG_HEADER "")
foreach(ZHLN_SVG_CANDIDATE_DIR IN LISTS ZHLN_SVG_HEADER_DIR)
    if(EXISTS "${ZHLN_SVG_CANDIDATE_DIR}/resvg/resvg.h")
        set(ZHLN_SVG_HEADER "${ZHLN_SVG_CANDIDATE_DIR}/resvg/resvg.h")
        break()
    elseif(EXISTS "${ZHLN_SVG_CANDIDATE_DIR}/resvg.h")
        set(ZHLN_SVG_HEADER "${ZHLN_SVG_CANDIDATE_DIR}/resvg.h")
        break()
    endif()
endforeach()

if(ZHLN_SVG_HEADER)
    file(STRINGS "${ZHLN_SVG_HEADER}" ZHLN_SVG_VERSION_LINES REGEX "^#define RESVG_(MAJOR|MINOR|PATCH)_VERSION ")

    set(ZHLN_SVG_MAJOR "")
    set(ZHLN_SVG_MINOR "")
    set(ZHLN_SVG_PATCH "0")
    foreach(ZHLN_SVG_LINE IN LISTS ZHLN_SVG_VERSION_LINES)
        if(ZHLN_SVG_LINE MATCHES "^#define RESVG_MAJOR_VERSION[ \t]+([0-9]+)")
            set(ZHLN_SVG_MAJOR "${CMAKE_MATCH_1}")
        elseif(ZHLN_SVG_LINE MATCHES "^#define RESVG_MINOR_VERSION[ \t]+([0-9]+)")
            set(ZHLN_SVG_MINOR "${CMAKE_MATCH_1}")
        elseif(ZHLN_SVG_LINE MATCHES "^#define RESVG_PATCH_VERSION[ \t]+([0-9]+)")
            set(ZHLN_SVG_PATCH "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    if(NOT ZHLN_SVG_MAJOR STREQUAL "" AND NOT ZHLN_SVG_MINOR STREQUAL "")
        set(ZHLN_SVG_VERSION "${ZHLN_SVG_MAJOR}.${ZHLN_SVG_MINOR}.${ZHLN_SVG_PATCH}")
    endif()
endif()

# resvg 0.48.0 added SVGZ_UNSUPPORTED to resvg_error in lib.rs but not to the
# resvg.h committed beside it, and cargo-c installs that committed file verbatim
# (`[package.metadata.capi.header] generation = false`), so on such an install
# every error code from FILE_OPEN_FAILED up arrives one higher than the constant
# naming it. SVG.cpp measures the gap and closes it; reporting it here is what
# keeps that compensation from reading as a mystery in the build log.
set(ZHLN_SVG_HEADER_LAGS "")
if(ZHLN_SVG_HEADER AND ZHLN_SVG_VERSION VERSION_GREATER_EQUAL "0.48")
    file(STRINGS "${ZHLN_SVG_HEADER}" ZHLN_SVG_SVGZ_LINES REGEX "RESVG_ERROR_SVGZ_UNSUPPORTED")
    if(NOT ZHLN_SVG_SVGZ_LINES)
        set(ZHLN_SVG_HEADER_LAGS ", whose resvg.h is behind its library's resvg_error numbering -- SVG.cpp compensates")
    endif()
endif()

if(NOT ZHLN_SVG_VERSION STREQUAL "" AND ZHLN_SVG_VERSION VERSION_LESS ZHLN_SVG_MINIMUM_VERSION)
    message(WARNING
        "extras/SVG: found resvg ${ZHLN_SVG_VERSION} (${ZHLN_SVG_ORIGIN}), but "
        "${ZHLN_SVG_MINIMUM_VERSION} is the oldest C API SVG.cpp is written "
        "against, so zahlen_svg will not be built. Upgrade resvg, or pass "
        "-DZHLN_BUILD_SVG=OFF to silence this.")
    return()
endif()

# --- target ----------------------------------------------------------------

add_library(${TARGET_NAME} STATIC
    SVG.cpp
)

set_target_properties(${TARGET_NAME} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

# Core is a public dependency: SVG.hpp includes <Zahlen/Error.hpp> and every
# signature in it returns std::expected<T, ZHLN::Error>, so a consumer needs
# core's include path and its error-category registration, not just this
# archive.
target_link_libraries(${TARGET_NAME} PUBLIC
    zahlen_engine
)

# resvg stays PRIVATE, the way simdjson does in ../CMakeLists.txt: SVG.hpp names
# no resvg type and includes no resvg header, so neither the include path nor the
# library can leak into a consumer, and a resvg upgrade recompiles this one file.
target_link_libraries(${TARGET_NAME} PRIVATE
    ${ZHLN_SVG_TARGET}
)

# A static libresvg.a -- a cargo-c build installed by hand, say -- carries the
# Rust runtime's own dependencies, which a shared libresvg.so has already
# resolved inside itself. A shared find is left alone.
if(UNIX AND ZHLN_SVG_BARE_LIBRARY MATCHES "\\.a$")
    find_package(Threads QUIET)
    if(TARGET Threads::Threads)
        target_link_libraries(${TARGET_NAME} PRIVATE Threads::Threads)
    else()
        target_link_libraries(${TARGET_NAME} PRIVATE pthread)
    endif()
    target_link_libraries(${TARGET_NAME} PRIVATE
        ${CMAKE_DL_LIBS}
        m
    )
endif()

# Public headers are included as <SVG/SVG.hpp>, so the extras root is the only
# include path this target publishes.
target_include_directories(${TARGET_NAME} PUBLIC
    ${PROJECT_SOURCE_DIR}/extras
)

if(COMMAND zahlen_enable_reflection)
    zahlen_enable_reflection(${TARGET_NAME})
endif()

target_link_libraries(${TARGET_NAME} PRIVATE zahlen_warnings)

# Nothing links zahlen_svg automatically: unlike the editor and the scripting
# runtime, no part of the composition root consumes it yet, and an unused static
# library is still a resvg dependency at link time for whoever inherits it. A
# host that wants SVG links zahlen_svg itself, guarded on TARGET.
if(ZHLN_SVG_VERSION STREQUAL "")
    message(STATUS "extras/SVG: building zahlen_svg against resvg (${ZHLN_SVG_ORIGIN}), version not reported.")
else()
    message(STATUS "extras/SVG: building zahlen_svg against resvg ${ZHLN_SVG_VERSION} (${ZHLN_SVG_ORIGIN})${ZHLN_SVG_HEADER_LAGS}.")
endif()
