# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

# Document import is reusable by the scene host and the world editor only.
add_library(zahlen_scene_documents STATIC SceneDocument.cpp)
set_target_properties(zahlen_scene_documents PROPERTIES CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_scene_documents PUBLIC zahlen_scene zahlen_serialization
    PRIVATE zahlen_warnings)
target_include_directories(zahlen_scene_documents PUBLIC ${PROJECT_SOURCE_DIR}/utils/scene)
zahlen_enable_reflection(zahlen_scene_documents)

# Unlike the game executable, this opt-in host tests document loading,
# fallback and graphics-file watching (plus Discord when that target exists).
if(TARGET zahlen_fallback_scene)
    add_executable(zahlen_scene_host main.cpp)
    set_target_properties(zahlen_scene_host PROPERTIES CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME "zahlen-scene-host")
    target_link_libraries(zahlen_scene_host PRIVATE zahlen_application
        zahlen_scene_documents zahlen_fallback_scene zahlen_warnings)
    if(TARGET zahlen_console)
        target_link_libraries(zahlen_scene_host PRIVATE zahlen_console)
        target_compile_definitions(zahlen_scene_host PRIVATE ZHLN_SCENE_HOST_HAS_CONSOLE=1)
    endif()
    if(TARGET zahlen_terrain AND TARGET zahlen_vfx)
        target_link_libraries(zahlen_scene_host PRIVATE zahlen_terrain zahlen_vfx)
        target_compile_definitions(zahlen_scene_host PRIVATE ZHLN_SCENE_HOST_HAS_TERRAIN=1)
    endif()
    if(TARGET zahlen_discord_rpc)
        target_link_libraries(zahlen_scene_host PRIVATE zahlen_discord_rpc)
        target_compile_definitions(zahlen_scene_host PRIVATE ZHLN_SCENE_HOST_HAS_DISCORD=1)
    endif()
    zahlen_set_bin_output(zahlen_scene_host)
    zahlen_add_runtime_dlls(zahlen_scene_host)
    add_dependencies(zahlen_scene_host link_resources)
    zahlen_enable_reflection(zahlen_scene_host)
    if(USE_SANITIZERS)
        zahlen_install_sanitizer_runtime(zahlen_scene_host)
    endif()
endif()
