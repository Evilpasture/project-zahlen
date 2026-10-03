# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/AssetPipeline.cmake
#
# The offline asset pipeline: the zcook cooker and the parallel Ninja graph it
# writes for every asset folder.
#
# zcook lives in tools/, not src/: it is the offline asset pipeline, and the
# off-line GLB emitter consumes plugins/json's reflection serializer. Core
# (src/, include/, modules/) must never depend on optional layers -- see
# configure/check_core_layer_boundary.py -- so the cooker is outside core roots.
#
# This module defines the zcook target and the zahlen_configure_game_assets()
# helper. The helper does add_custom_command(TARGET zcook ...) and
# add_dependencies(zahlen cook_assets), so the root CMakeLists calls it only
# after both targets exist.

# Source-image and ZRD1/ZRD2 codecs are optional to the engine, but always needed
# by the offline cooker. Keep them in the asset-cooking plugin target so an application (e.g.
# FidelityHarness) can explicitly supply decoded pixels without pulling any
# file formats into the runtime libraries.
add_subdirectory(plugins/AssetCooking)
add_executable(zcook
    tools/zcook/main.cpp
    tools/zcook/Transform.cpp
    tools/zcook/GLB.cpp
    tools/zcook/Cook.cpp
    tools/zcook/Ninja.cpp
    tools/zcook/FontBake.cpp
)
target_link_libraries(zcook PRIVATE zahlen_engine zahlen_filesystem zahlen_threading zahlen_asset_cooking)
target_include_directories(zcook SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/plugins
    ${CMAKE_SOURCE_DIR}/tools/zcook
)

set(ZHLN_SHARED_ASSET_DIR "${CMAKE_SOURCE_DIR}/build/shared_assets" CACHE PATH "Shared cooked asset cache")

function(zahlen_configure_game_assets GAME_SOURCE_DIR)
    file(MAKE_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}")

    set(SHARED_ZCOOK "${ZHLN_SHARED_ASSET_DIR}/zcook${CMAKE_EXECUTABLE_SUFFIX}")

    # 1. Symlink the current build's zcook to a STABLE shared location
    add_custom_command(
        TARGET zcook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink "$<TARGET_FILE:zcook>" "${SHARED_ZCOOK}"
        COMMENT "Updating stable shared zcook symlink..."
    )

    # 2. Generate assets.ninja with zcook itself, through the FIXED path: the
    #    cooker writes the graph of its own invocations, so the rules and the
    #    subcommands they name cannot drift apart. It depends on the zcook target
    #    because the generator IS zcook -- the symlink above has to exist first.
    #
    #    Two spellings of the same command, because the prefix that lets zcook
    #    start on Windows (see the note in HostToolRuntime.cmake) cannot be empty
    #    in the other one: an empty QUOTED argument becomes the command's first
    #    word, and CMake drops a command whose program is empty -- silently. On
    #    every other platform the command is exactly what it always was.
    if(ZHLN_TOOL_ENV_COMMAND)
        add_custom_command(
            OUTPUT "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
            COMMAND ${ZHLN_TOOL_ENV_COMMAND} "${ZHLN_TOOL_ENV_ARGUMENT}" "${SHARED_ZCOOK}" ninja
                    --out "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
                    --source "${GAME_SOURCE_DIR}"
                    --engine-tools "${zahlen_SOURCE_DIR}/tools"
                    --self "${SHARED_ZCOOK}"
            WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
            DEPENDS zcook
                    "${zahlen_SOURCE_DIR}/tools/export_metadata.py"
                    "${zahlen_SOURCE_DIR}/tools/run_blender.py"
            COMMENT "Scanning asset folders and generating assets.ninja..."
        )
    else()
        add_custom_command(
            OUTPUT "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
            COMMAND "${SHARED_ZCOOK}" ninja
                    --out "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
                    --source "${GAME_SOURCE_DIR}"
                    --engine-tools "${zahlen_SOURCE_DIR}/tools"
                    --self "${SHARED_ZCOOK}"
            WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
            DEPENDS zcook
                    "${zahlen_SOURCE_DIR}/tools/export_metadata.py"
                    "${zahlen_SOURCE_DIR}/tools/run_blender.py"
            COMMENT "Scanning asset folders and generating assets.ninja..."
        )
    endif()

    # 3. Use symlink for base.pak
    if(WIN32)
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E copy "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    else()
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E create_symlink "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    endif()

    # The target around the cooking step. The nested ninja it starts is what
    # launches one zcook per asset, so the loader path has to be on this command
    # too and not only on the one above: children inherit it. Same two spellings
    # as above, for the same reason -- and the PATH argument stays written out
    # here rather than being carried in a variable, because the escapes that make
    # it one argument do not survive a list.
    set(COOK_ASSETS_PREFIX
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zahlen>/data"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZHLN_SHARED_ASSET_DIR}/build_assets"
    )
    if(ZHLN_TOOL_ENV_COMMAND)
        add_custom_target(cook_assets ALL
            ${COOK_ASSETS_PREFIX}
            # Run ninja inside the shared directory with persistent .ninja_log
            COMMAND ${ZHLN_TOOL_ENV_COMMAND} "${ZHLN_TOOL_ENV_ARGUMENT}"
                    ninja -C "${ZHLN_SHARED_ASSET_DIR}" -f "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
            COMMAND ${PAK_LINK_CMD}
            DEPENDS "${ZHLN_SHARED_ASSET_DIR}/assets.ninja" zcook
            WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
            COMMENT "Cooking assets in parallel using Ninja..."
            USES_TERMINAL
        )
    else()
        add_custom_target(cook_assets ALL
            ${COOK_ASSETS_PREFIX}
            # Run ninja inside the shared directory with persistent .ninja_log
            COMMAND ninja -C "${ZHLN_SHARED_ASSET_DIR}" -f "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
            COMMAND ${PAK_LINK_CMD}
            DEPENDS "${ZHLN_SHARED_ASSET_DIR}/assets.ninja" zcook
            WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
            COMMENT "Cooking assets in parallel using Ninja..."
            USES_TERMINAL
        )
    endif()
    add_dependencies(zahlen cook_assets)
endfunction()
