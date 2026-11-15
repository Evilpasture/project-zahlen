# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# utils/zcook/CMakeLists.txt
#
# The cooker. The parallel Ninja graph it writes, and the custom commands that
# drive it per asset folder, stay in cmake/AssetPipeline.cmake -- this file owns
# the executable only.

set(ZHLN_ZCOOK_SOURCES
    main.cpp
    Transform.cpp
    GLB.cpp
    Cook.cpp
    Ninja.cpp
    FontBake.cpp
)

add_executable(zcook ${ZHLN_ZCOOK_SOURCES})

# Only the engine and the cooking helper. zcook reads FS::PakHeader/PakEntry
# (POD structs in the public VFS.hpp) and calls TaskSystem::ParallelFor, whose
# only out-of-line callee is Dispatch -- both exported by libzahlen_engine.so.
# The archives were linked ahead of the .so, so the executable ran a second copy
# of the scheduler next to the one the engine uses.
target_link_libraries(zcook PRIVATE zahlen_engine zahlen_asset_cooking)

# main.cpp calls Reflect::EnumToString, and ErrorCode's names come from the
# reflection configuration, so the cooker is one of the targets that reads a
# reflected header.
zahlen_enable_reflection(zcook)

zahlen_set_bin_output(zcook)
zahlen_add_runtime_dlls(zcook)

# The stable shared-cook location, set up by cmake/AssetPipeline.cmake. The
# POST_BUILD step lives here because add_custom_command(TARGET ...) has to run
# in the directory that created the target, and zahlen_configure_game_assets()
# is called from the root CMakeLists.
if(DEFINED ZHLN_SHARED_ASSET_DIR)
    add_custom_command(
        TARGET zcook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE:zcook>"
                "${ZHLN_SHARED_ASSET_DIR}/zcook${CMAKE_EXECUTABLE_SUFFIX}"
        COMMENT "Updating stable shared zcook symlink..."
    )
endif()

target_include_directories(zcook SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/extern/cgltf
    ${CMAKE_SOURCE_DIR}/extern/stb
    ${CMAKE_SOURCE_DIR}/plugins
    ${CMAKE_CURRENT_SOURCE_DIR}
)
