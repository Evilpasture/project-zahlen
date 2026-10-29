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
    SystemWiring.cpp
    Hierarchy.cpp
    PrefabFactory.cpp
    MeshBuilder.cpp
    Meshlet.cpp
    AssetManager.cpp
    Platform.cpp
    Profiler.cpp
    system/RenderSystem.cpp
    system/GraphicsSettingsSync.cpp
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
    target_sources(zahlen_engine INTERFACE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:$<TARGET_OBJECTS:zahlen_sanitizer_options>>
    )
endif()

target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

add_subdirectory(tty)
