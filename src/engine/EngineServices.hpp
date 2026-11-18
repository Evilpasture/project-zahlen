// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ECS/Entity.hpp>
#include <ecs/ExtensionServices.hpp>
#include <engine/PoseUploads.hpp>
#include <type_traits>

namespace ZHLN {

class RenderContext;
class VisibilityList;
struct SceneData;
struct CullingStats;
class AssetManager;
class PhysicsContext;
class AudioContext;
class ArticulationContext;
class InputContext;

struct DebugSettings {
    int physicsDrawMode = 0;
};

struct VisibleEntities {
    VisibilityList& values;
};
struct VisibleShadowEntities {
    VisibilityList& values;
};

namespace ECS {
class EventBus;
} // namespace ECS

struct SimServices {
    PhysicsContext&         physics;
    AudioContext&           audio;
    AssetManager&           assets;
    InputContext&           input;
    ArticulationContext&    articulation;
    ECS::EventBus&          events;
    ECS::ExtensionServices& extensions;
    PoseUploadQueue&        poseUploads;
};

struct RenderServices {
    RenderContext&          render;
    SceneData&              scene;
    AssetManager&           assets;
    InputContext&           input;
    ECS::ExtensionServices& extensions;
    CullingStats&           cullingStats;
    DebugSettings&          debugSettings;
    VisibleEntities         visible;
    VisibleShadowEntities   shadow;
};

namespace ECS {

template <> struct IsWithheldService<RenderContext> : std::true_type {};
template <> struct IsWithheldService<PhysicsContext> : std::true_type {};
template <> struct IsWithheldService<AudioContext> : std::true_type {};
template <> struct IsWithheldService<SceneData> : std::true_type {};

template <typename Services>
class SystemGraph;

using SimGraph    = SystemGraph<SimServices>;
using RenderGraph = SystemGraph<RenderServices>;
} // namespace ECS

} // namespace ZHLN
