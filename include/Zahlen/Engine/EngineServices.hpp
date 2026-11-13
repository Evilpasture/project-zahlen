// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine's two service bundles, and the one door between them.
//
// A graph is built with the services of its domain: the simulation graph gets
// SimServices, the render graph gets RenderServices. A system's signature names
// the services it uses, and a system that asks for something its graph does not
// provide fails to *compile* (see SystemGraph<Services>::AddSystem). That is
// what stops this header's types from ending up on a shared, untyped carrier:
// there is nothing to put them on.

#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/ECS/ExtensionServices.hpp>

namespace ZHLN {

class RenderContext;
class VisibilityList;
struct SceneData;
struct CullingStats;
class AssetManager;
class PhysicsContext;
class AudioContext;
class InputContext;
class ArticulationSystem;

struct DebugSettings {
    int physicsDrawMode = 0;
};

// Distinct parameter types for the two otherwise identically typed output
// lists. They are bundle members of the render domain, so a system resolves one
// by its type rather than by position. `values` is the engine-owned producer,
// never a bare container reference: a culling system writes through Clear/Add/
// AddRange, and everything else sees a std::span<const Entity>.
struct VisibleEntities {
    VisibilityList& values;
};
struct VisibleShadowEntities {
    VisibilityList& values;
};

namespace ECS {
class EventBus;
} // namespace ECS

// What the simulation graph provides. References: the engine owns these for its
// whole life, so a system never sees a null service and a resolver never checks
// for one. The simulation domain has no renderer -- see PoseUploadQueue for the
// one channel it has out.
struct SimServices {
    PhysicsContext&         physics;
    AudioContext&           audio;
    AssetManager&           assets;
    InputContext&           input;
    ArticulationSystem&     articulation;
    ECS::EventBus&          events;
    ECS::ExtensionServices& extensions;
    PoseUploadQueue&        poseUploads;
};

// What the render graph provides. `render` is here and *only* here: a
// simulation system cannot name it, which is the point.
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
template <typename Services>
class SystemGraph;

using SimGraph    = SystemGraph<SimServices>;
using RenderGraph = SystemGraph<RenderServices>;
} // namespace ECS

} // namespace ZHLN
