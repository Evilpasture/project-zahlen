# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Engine implementation. This is the game-facing orchestration layer: scene
# assembly, systems and platform services. The Vulkan backend, the rendering
# passes and the windowing subsystem own their source lists in sibling
# directories -- Window itself lives in src/window/, which is where GLFW is
# linked and where the presentation bridge is published from.
set(RAW_ENGINE_SOURCES
    # AssertHandler.cpp used to hold all of this in one 670-line file. Logging
    # and the crash path are separate concerns with different rules -- the crash
    # path cannot allocate -- so they live in separate translation units.
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
    FileSystemWatcher.cpp
    CreativeWorksFactory.cpp
    system/TextureSystem.cpp
    system/AnimationSystem.cpp
    CreativeWorksManager.cpp
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
    RuntimePaths.cpp
    system/TargetCameraSystem.cpp
    system/TransformSystem.cpp
    system/PhysicsStateSystem.cpp
    system/LightingSystem.cpp
    system/CameraSystem.cpp
    Scene.cpp
    ScriptRunner.cpp
)

# Reflection-capable compilers compile these files directly; other compilers
# receive their generated counterparts. Keep the ownership of this source list
# here so the root only composes engine subsystems.
zahlen_transpile_sources(ENGINE_SOURCES ${RAW_ENGINE_SOURCES})
target_sources(zahlen_engine PRIVATE ${ENGINE_SOURCES})

# Transpiled sources live under the build tree, where quote includes no longer
# resolve beside their original files. Engine and system-private headers must
# therefore remain available for both source variants.
target_include_directories(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/system
)

add_subdirectory(tty)
