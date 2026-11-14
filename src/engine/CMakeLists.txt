# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# This list is exhaustive on purpose -- nothing here globs. A source the list
# does not name is not compiled, and that failure mode is quiet: the symbols it
# should have defined stay undefined, the shared library still links (undefined
# symbols are allowed there), and only the callers that happen to sit in an
# archive linked into an executable report them. Add the file here in the same
# commit that adds the file; the link error that says otherwise names a target
# two steps away from the omission.

set(RAW_ENGINE_SOURCES
    Log.cpp
    diagnostics/StackTrace.cpp
    diagnostics/MemoryInspector.cpp
    diagnostics/SanitizerBridges.cpp
    diagnostics/RenderDocInit.cpp
    Engine.cpp
    EngineGlobals.cpp
    InputContext.cpp
    Kernel.cpp
    World.cpp
    VisibilityList.cpp
    Capabilities.cpp
    EngineCOM.cpp
    SystemWiring.cpp
    Hierarchy.cpp
    PrefabFactory.cpp
    MeshBuilder.cpp
    Meshlet.cpp
    AssetManager.cpp
    Platform.cpp
    Profiler.cpp
    system/RenderSystem.cpp
    system/DecalSystem.cpp
    system/AudioSystem.cpp
    system/ArticulationSystem.cpp
    system/CullingSystem.cpp
    system/PhysicsSystem.cpp
    system/LODSystem.cpp
    system/SceneResources.cpp
    system/SceneCleanupSystem.cpp
    FramePlan.cpp
    system/TransformSystem.cpp
    system/PhysicsStateSystem.cpp
    system/LightingSystem.cpp
    system/EnvironmentSunSystem.cpp
    system/CameraSystem.cpp
    Scene.cpp
)

zahlen_transpile_sources(ENGINE_SOURCES ${RAW_ENGINE_SOURCES})
target_sources(zahlen_engine PRIVATE ${ENGINE_SOURCES})

if(USE_SANITIZERS)
    add_library(zahlen_sanitizer_options OBJECT diagnostics/SanitizerOptions.cpp)
    set_target_properties(zahlen_sanitizer_options PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(zahlen_sanitizer_options PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
    )
    # The rules are #embed-ed from the paths exported here, the same way the
    # default font reaches src/gui/FontLoader.cpp: absolute, so the directive
    # cannot drift when a file moves, and never a run-time lookup. Only one
    # level of backslash is needed around the quotes -- "PATH=\"${path}\"" reaches
    # the compiler as -DPATH="/abs/path", which is a string literal; the doubled
    # form emits -DPATH=\\\"/abs/path\\\" and fails to expand (see the note in
    # SanitizerOptions.cpp). OBJECT_DEPENDS is what rebuilds this object when a
    # rule changes: a preprocessor dependency file is not guaranteed to list an
    # #embed-ed file.
    foreach(rule_file IN ITEMS lsan.supp ubsan.supp)
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${rule_file}")
            message(FATAL_ERROR "zahlen_sanitizer_options: '${rule_file}' is missing from the project root")
        endif()
    endforeach()
    # Both definitions travel as one ;-list value: PROPERTIES pairs names with
    # values, so a second bare argument would be read as another property name.
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/SanitizerOptions.cpp" PROPERTIES
        COMPILE_DEFINITIONS "ZHLN_LSAN_SUPPRESSIONS_PATH=\"${PROJECT_SOURCE_DIR}/lsan.supp\";ZHLN_UBSAN_SUPPRESSIONS_PATH=\"${PROJECT_SOURCE_DIR}/ubsan.supp\""
        OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/lsan.supp;${PROJECT_SOURCE_DIR}/ubsan.supp"
    )
    target_sources(zahlen_engine INTERFACE
        $<BUILD_INTERFACE:$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<TARGET_OBJECTS:zahlen_sanitizer_options>>>
    )
endif()

target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

add_subdirectory(tty)
