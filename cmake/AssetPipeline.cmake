# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/AssetPipeline.cmake
#
# The offline asset pipeline: the zcook cooker and the parallel Ninja graph it
# writes for every asset folder.
#
# zcook lives in tools/, not src/: it is the offline asset pipeline, and the
# off-line GLB emitter consumes extras/json's reflection serializer. Core
# (src/, include/, modules/) must never depend on extras -- see
# configure/check_core_extras_boundary.py -- so the cooker is outside core roots.
#
# This module defines the zcook target and the zahlen_configure_game_assets()
# helper. The helper does add_custom_command(TARGET zcook ...) and
# add_dependencies(zahlen cook_assets), so the root CMakeLists calls it only
# after both targets exist.

add_executable(zcook
    tools/zcook/main.cpp
    tools/zcook/Transform.cpp
    tools/zcook/GLB.cpp
    tools/zcook/Cook.cpp
    tools/zcook/Ninja.cpp
    tools/zcook/FontBake.cpp
)
target_link_libraries(zcook PRIVATE zahlen_engine)
target_include_directories(zcook SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/extras
    ${CMAKE_SOURCE_DIR}/tools/zcook
)

set(ZHLN_SHARED_ASSET_DIR "${CMAKE_SOURCE_DIR}/build/shared_assets" CACHE PATH "Shared cooked asset cache")

function(zahlen_configure_game_assets GAME_SOURCE_DIR)
    file(MAKE_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}")

    set(SHARED_ZCOOK "${ZHLN_SHARED_ASSET_DIR}/zcook")

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

    # 3. Use symlink for base.pak
    if(WIN32)
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E copy "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    else()
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E create_symlink "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    endif()

    add_custom_target(cook_assets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zahlen>/data"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZHLN_SHARED_ASSET_DIR}/build_assets"
        # Run ninja inside the shared directory with persistent .ninja_log
        COMMAND ninja -C "${ZHLN_SHARED_ASSET_DIR}" -f "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
        COMMAND ${PAK_LINK_CMD}
        DEPENDS "${ZHLN_SHARED_ASSET_DIR}/assets.ninja" zcook
        WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
        COMMENT "Cooking assets in parallel using Ninja..."
        USES_TERMINAL
    )
    add_dependencies(zahlen cook_assets)
endfunction()
