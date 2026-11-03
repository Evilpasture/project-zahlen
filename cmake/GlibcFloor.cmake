# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/GlibcFloor.cmake
#
# The release claims a glibc floor -- "runs on Ubuntu 22.04, glibc 2.35" -- and
# that claim is a property of the artifacts this build produces, not of the
# tarballs made from them. Until now it was checked only by
# scripts/check-glibc-floor.sh, called from five shell scripts and two workflow
# steps and, deliberately, from nowhere in CMake. So the floor was verified
# exactly once per release, on packaged archives, after the build and the
# signing, and a regression introduced by a developer on a newer distro was
# invisible until that release run -- days later, in a lane that then could not
# say which change did it.
#
# ZHLN_GLIBC_BASELINE is empty by default on purpose: on a host whose glibc is
# newer than the floor, the artifacts legitimately carry the newer symbols, and a
# developer building there is not doing anything wrong. The release lane sets it,
# because there it is not advice but the product's contract.
#
# This is a build target rather than an add_test(): tests in this tree run through
# `ztest`, not CTest (see cmake/ZTest.cmake -- "replaces CTest across the
# repository"); CTest is used only by the zpkg subproject. The entry point is
#
#     cmake --build <build-dir> --target glibc-floor

include_guard(GLOBAL)

set(ZHLN_GLIBC_BASELINE "" CACHE STRING
    "Maximum glibc symbol version this build's artifacts may require (e.g. 2.35). Empty disables the check.")

set(_ZHLN_GLIBC_FLOOR_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../scripts/check-glibc-floor.sh")

# zhln_add_glibc_floor_check(<target>...)
#
# Call once, with the targets whose files actually ship. Targets that do not exist
# in this configuration are skipped, so the call site can name the whole release
# without guarding each name; only shared objects and executables are checked,
# since a static archive carries no symbol versions and would check nothing.
function(zhln_add_glibc_floor_check)
    if(NOT ZHLN_GLIBC_BASELINE)
        return()
    endif()
    if(NOT EXISTS "${_ZHLN_GLIBC_FLOOR_SCRIPT}")
        message(WARNING "glibc floor: ${_ZHLN_GLIBC_FLOOR_SCRIPT} is missing; the check was not added")
        return()
    endif()
    set(_artifacts "")
    set(_deps "")
    foreach(_target IN LISTS ARGN)
        if(TARGET ${_target})
            get_target_property(_type ${_target} TYPE)
            if(_type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
                list(APPEND _artifacts "$<TARGET_FILE:${_target}>")
                list(APPEND _deps ${_target})
            endif()
        endif()
    endforeach()
    if(NOT _artifacts)
        message(STATUS "glibc floor: nothing that ships in this configuration, check not added")
        return()
    endif()
    add_custom_target(glibc-floor
        COMMAND bash "${_ZHLN_GLIBC_FLOOR_SCRIPT}" "${ZHLN_GLIBC_BASELINE}" ${_artifacts}
        DEPENDS ${_deps}
        COMMENT "glibc floor ${ZHLN_GLIBC_BASELINE}: ${_deps}"
        VERBATIM)
    message(STATUS "glibc floor: `--target glibc-floor` holds ${_deps} to ${ZHLN_GLIBC_BASELINE}")
endfunction()
