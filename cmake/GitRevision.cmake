# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/GitRevision.cmake
#
# --- RETRIEVE GIT COMMIT HASH ---
# ZHLN_GIT_HASH is embedded in the engine as ZHLN_GIT_COMMIT_HASH. A describe
# that names a tag beats a bare rev-parse, so releases report "v1.2-3-gabc"
# rather than "abc". Without Git (a source tarball) the hash is "unknown".

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE ZHLN_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT ZHLN_GIT_HASH)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            OUTPUT_VARIABLE ZHLN_GIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
    endif()
endif()

if(NOT ZHLN_GIT_HASH)
    set(ZHLN_GIT_HASH "unknown")
endif()
