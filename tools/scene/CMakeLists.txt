# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Document import is reusable by the scene host and the world editor only.
add_library(zahlen_scene_documents STATIC SceneDocument.cpp)
set_target_properties(zahlen_scene_documents PROPERTIES CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zahlen_scene_documents PUBLIC zahlen_engine zahlen_serialization
    PRIVATE zahlen_warnings)
target_include_directories(zahlen_scene_documents PUBLIC ${PROJECT_SOURCE_DIR}/tools/scene)
target_compile_definitions(zahlen_scene_documents PRIVATE JPH_DOUBLE_PRECISION)
zahlen_enable_reflection(zahlen_scene_documents)
if(TARGET zahlen_lua)
    target_link_libraries(zahlen_scene_documents PRIVATE zahlen_lua)
    target_compile_definitions(zahlen_scene_documents PRIVATE ZHLN_SCENE_DOC_HAS_LUA=1)
endif()

# Unlike the game executable, this opt-in host tests document loading,
# fallback and graphics-file watching (plus Discord when that target exists).
if(TARGET zahlen_fallback_scene)
    add_executable(zahlen_scene_host main.cpp)
    set_target_properties(zahlen_scene_host PROPERTIES CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF OUTPUT_NAME "zahlen-scene-host")
    target_link_libraries(zahlen_scene_host PRIVATE zahlen_application
        zahlen_scene_documents zahlen_fallback_scene zahlen_warnings)
    target_include_directories(zahlen_scene_host PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_compile_definitions(zahlen_scene_host PRIVATE JPH_DOUBLE_PRECISION)
    if(TARGET zahlen_lua)
        target_link_libraries(zahlen_scene_host PRIVATE zahlen_lua)
        target_compile_definitions(zahlen_scene_host PRIVATE ZHLN_SCENE_HOST_HAS_LUA=1)
    endif()
    if(TARGET zahlen_discord_engine)
        target_link_libraries(zahlen_scene_host PRIVATE zahlen_discord_engine)
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
