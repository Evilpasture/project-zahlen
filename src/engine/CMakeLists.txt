# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Engine implementation. This is the game-facing orchestration layer: window
# lifecycle, scene assembly, systems and platform services. The Vulkan backend
# and rendering passes own their source lists in sibling directories.
set(RAW_ENGINE_SOURCES
    AssertHandler.cpp
    SignalManager.cpp
    DefaultPreset.cpp
    Engine.cpp
    FileSystemWatcher.cpp
    CreativeWorksFactory.cpp
    system/TextureSystem.cpp
    system/AnimationSystem.cpp
    CreativeWorksManager.cpp
    Window.cpp
    Platform.cpp
    Profiler.cpp
    system/RenderSystem.cpp
    system/GraphicsSettingsSync.cpp
    system/DecalSystem.cpp
    system/MovementSystem.cpp
    system/AudioSystem.cpp
    system/ArticulationSystem.cpp
    system/CullingSystem.cpp
    system/PhysicsSystem.cpp
    system/TerrainSystem.cpp
    system/LODSystem.cpp
    system/ParticleSystem.cpp
    CommandLine.cpp
    system/TargetCameraSystem.cpp
    system/TransformSystem.cpp
    system/PhysicsStateSystem.cpp
    system/LightingSystem.cpp
    system/InputSystem.cpp
    system/CameraSystem.cpp
    system/InteractionSystem.cpp
    WindowSurface.cpp
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
