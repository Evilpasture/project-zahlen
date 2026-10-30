# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/BlenderProbe.cmake
#
# Optional Blender detection (`ZHLN_HAS_BLENDER`).
#
# .blend sources are the one asset format this tree cannot open by itself: the
# offline pipeline hands each one to the host's Blender (`zcook blender`, which
# scrubs the host Python environment first) and cooks whatever the exporter's
# manifest lists. Blender is a 385 MiB package and needs a matching bundled
# Python, and a tree with no .blend sources -- this one, for instance -- has no
# use for it at all. So it is optional:
#   - ZHLN_HAS_BLENDER=TRUE  -> the generated asset graph gains one
#                               blender_extract edge per .blend source.
#   - ZHLN_HAS_BLENDER=FALSE -> .blend sources are skipped; every other format
#                               (ztex, zfont, glb, pak) still cooks.
# Pass `-DZHLN_HAS_BLENDER=OFF` to take the Blender-free graph on a host that
# has Blender installed, or `-DZHLN_HAS_BLENDER=ON` to require it.
#
# Discovery order matches FindBlenderExecutable() in tools/zcook/Ninja.cpp --
# BLENDER_PATH, then PATH, then the platform's own install locations -- so
# configure and cook cannot disagree about which Blender, or whether there is
# one. The path this settles on is passed to the generator (cmake/
# AssetPipeline.cmake), which names it in the rule it writes, so the answer
# survives into the build instead of being probed a second time there.

include_guard(GLOBAL)

set(_zhln_blender_forced_off FALSE)
set(_zhln_blender_forced_on FALSE)
if(DEFINED ZHLN_HAS_BLENDER)
    if(ZHLN_HAS_BLENDER)
        set(_zhln_blender_forced_on TRUE)
    else()
        set(_zhln_blender_forced_off TRUE)
    endif()
endif()

if(_zhln_blender_forced_off)
    set(ZHLN_HAS_BLENDER FALSE)
    set(ZHLN_BLENDER_EXECUTABLE "")
else()
    # BLENDER_PATH names the executable, not a directory, so it is tested as a
    # file rather than handed to find_program as a search path.
    if(DEFINED ENV{BLENDER_PATH} AND EXISTS "$ENV{BLENDER_PATH}")
        set(ZHLN_BLENDER_EXECUTABLE "$ENV{BLENDER_PATH}")
    else()
        # "Blender" is the macOS bundle's spelling, one level below the app.
        find_program(ZHLN_BLENDER_EXECUTABLE NAMES blender Blender
            HINTS /Applications/Blender.app/Contents/MacOS)
    endif()

    # Windows installs Blender under Program Files and puts nothing on PATH. The
    # newest "Blender Foundation/Blender <version>" directory is the answer
    # there -- the same scan zcook performs when it launches it.
    if(NOT ZHLN_BLENDER_EXECUTABLE AND WIN32)
        file(GLOB _zhln_blender_installs LIST_DIRECTORIES true
            "C:/Program Files/Blender Foundation/Blender*")
        list(SORT _zhln_blender_installs)
        list(REVERSE _zhln_blender_installs)
        foreach(_zhln_blender_dir IN LISTS _zhln_blender_installs)
            if(EXISTS "${_zhln_blender_dir}/blender.exe")
                set(ZHLN_BLENDER_EXECUTABLE "${_zhln_blender_dir}/blender.exe")
                break()
            endif()
        endforeach()
        unset(_zhln_blender_dir)
        unset(_zhln_blender_installs)
    endif()

    if(ZHLN_BLENDER_EXECUTABLE AND EXISTS "${ZHLN_BLENDER_EXECUTABLE}")
        set(ZHLN_HAS_BLENDER TRUE)
    else()
        unset(ZHLN_BLENDER_EXECUTABLE CACHE)
        unset(ZHLN_BLENDER_EXECUTABLE)
        if(_zhln_blender_forced_on)
            message(FATAL_ERROR
                "ZHLN_HAS_BLENDER=ON was requested, but no Blender executable was found. "
                "Install Blender, point BLENDER_PATH at it, or pass -DZHLN_HAS_BLENDER=OFF "
                "to skip .blend sources.")
        endif()
        set(ZHLN_HAS_BLENDER FALSE)
    endif()
endif()

if(ZHLN_HAS_BLENDER)
    message(STATUS "Blender: ${ZHLN_BLENDER_EXECUTABLE} (.blend sources will be cooked)")
    add_compile_definitions(ZHLN_HAS_BLENDER=1)
elseif(_zhln_blender_forced_off)
    message(STATUS "Blender: disabled by ZHLN_HAS_BLENDER=OFF (.blend sources are skipped)")
else()
    message(STATUS
        "Blender: not found, .blend sources are skipped (ztex/zfont/glb/pak still cook). "
        "Set BLENDER_PATH or -DZHLN_HAS_BLENDER=ON to cook them.")
endif()
