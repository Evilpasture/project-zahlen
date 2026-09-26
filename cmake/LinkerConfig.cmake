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

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE ZHLN_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ZHLN_TARGET_TRIPLE)
    set(ZHLN_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-unknown-${CMAKE_SYSTEM_NAME}")
    string(TOLOWER "${ZHLN_TARGET_TRIPLE}" ZHLN_TARGET_TRIPLE)
endif()

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=mold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE MOLD_VERSION)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=lld -Wl,--version ERROR_QUIET OUTPUT_VARIABLE LLD_VERSION)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=gold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE GOLD_VERSION)

if(MOLD_VERSION MATCHES "mold")
    message(STATUS "Using MOLD linker")
    add_link_options(-fuse-ld=mold)
    set(ZHLN_LINKER "mold")
elseif(LLD_VERSION MATCHES "LLD")
    message(STATUS "Using LLD linker")
    add_link_options(-fuse-ld=lld)
    set(ZHLN_LINKER "lld")
elseif(GOLD_VERSION MATCHES "GNU gold")
    message(STATUS "Using GOLD linker")
    add_link_options(-fuse-ld=gold)
    set(ZHLN_LINKER "gold")
else()
    message(STATUS "Using default BFD linker")
    set(ZHLN_LINKER "bfd")
endif()
if(UNIX AND NOT APPLE)
    add_link_options(-rdynamic)
endif()
