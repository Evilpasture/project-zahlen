# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

set(RAW_ENGINE_SOURCES
    Log.cpp
    diagnostics/CrashHandler.cpp
    diagnostics/StackTrace.cpp
    diagnostics/MemoryInspector.cpp
    diagnostics/SanitizerBridges.cpp
    SignalManager.cpp
    Engine.cpp
    EngineGlobals.cpp
    Kernel.cpp
    World.cpp
    SystemWiring.cpp
    Hierarchy.cpp
    PrefabFactory.cpp
    system/TextureSystem.cpp
    system/AnimationSystem.cpp
    AssetManager.cpp
    RadianceMap.cpp
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
    system/ParticleSystem.cpp
    CommandLine.cpp
    system/TransformSystem.cpp
    system/PhysicsStateSystem.cpp
    system/LightingSystem.cpp
    system/CameraSystem.cpp
    Scene.cpp
    ScriptRunner.cpp
)

zahlen_transpile_sources(ENGINE_SOURCES ${RAW_ENGINE_SOURCES})
target_sources(zahlen_engine PRIVATE ${ENGINE_SOURCES})

target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

add_subdirectory(tty)
