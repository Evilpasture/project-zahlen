# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/LinkerConfig.cmake
#
# --- LINKER DETECTION & TARGET TRIPLE ---
# The fastest linker on the machine wins: mold, then lld, then gold, then the
# default BFD. The choice is embedded in the engine (ZHLN_LINKER_NAME) so a
# binary can report how it was linked; ZHLN_TARGET_TRIPLE comes from the
# compiler itself (-dumpmachine) rather than from CMake's view of the machine.
# Must be included before any add_subdirectory: the link options below only
# reach targets that are created afterwards.
#
# Results are cached against CMAKE_CXX_COMPILER so warm reconfigures do not
# spawn 4 compiler/linker subprocesses, and cold configures short-circuit on
# the first fastest available linker.

if(NOT ZHLN_CACHED_LINKER_COMPILER STREQUAL "${CMAKE_CXX_COMPILER}"
   OR NOT ZHLN_CACHED_TARGET_TRIPLE
   OR NOT ZHLN_CACHED_LINKER)
    execute_process(
        COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
        OUTPUT_VARIABLE _zhln_triple
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _zhln_triple)
        set(_zhln_triple "${CMAKE_SYSTEM_PROCESSOR}-unknown-${CMAKE_SYSTEM_NAME}")
        string(TOLOWER "${_zhln_triple}" _zhln_triple)
    endif()

    set(_zhln_linker "bfd")
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=mold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE _mold_ver)
    if(_mold_ver MATCHES "mold")
        set(_zhln_linker "mold")
    else()
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=lld -Wl,--version ERROR_QUIET OUTPUT_VARIABLE _lld_ver)
        if(_lld_ver MATCHES "LLD")
            set(_zhln_linker "lld")
        else()
            execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=gold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE _gold_ver)
            if(_gold_ver MATCHES "GNU gold")
                set(_zhln_linker "gold")
            endif()
        endif()
    endif()

    set(ZHLN_CACHED_LINKER_COMPILER "${CMAKE_CXX_COMPILER}" CACHE INTERNAL "Compiler used for linker probe")
    set(ZHLN_CACHED_TARGET_TRIPLE "${_zhln_triple}" CACHE INTERNAL "Detected target triple")
    set(ZHLN_CACHED_LINKER "${_zhln_linker}" CACHE INTERNAL "Detected fastest linker")
endif()

set(ZHLN_TARGET_TRIPLE "${ZHLN_CACHED_TARGET_TRIPLE}")
set(ZHLN_LINKER "${ZHLN_CACHED_LINKER}")

if(ZHLN_LINKER STREQUAL "mold")
    message(STATUS "Using MOLD linker")
    add_link_options(-fuse-ld=mold)
elseif(ZHLN_LINKER STREQUAL "lld")
    message(STATUS "Using LLD linker")
    add_link_options(-fuse-ld=lld)
elseif(ZHLN_LINKER STREQUAL "gold")
    message(STATUS "Using GOLD linker")
    add_link_options(-fuse-ld=gold)
else()
    message(STATUS "Using default BFD linker")
endif()
if(UNIX AND NOT APPLE)
    add_link_options(-rdynamic)
endif()
