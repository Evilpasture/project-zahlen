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
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/InputHaptics.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/ECS/ExtensionServices.hpp>
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
//
// Input arrives as the two capabilities, never as the router. A system taking
// Res<InputReader> can only read; one taking ResMut<InputHaptics> can only pulse
// a pad (and gets an isolated ServiceToken<InputHaptics> write edge, so it does
// not serialize against readers). A system naming InputActions -- and therefore
// anything that could rebind a key mid-frame -- is rejected at
// AddSystem<...>() by the parameter-resolver gate, not at review time.
struct SimServices {
    PhysicsContext&         physics;
    AudioContext&           audio;
    // The capability, not the cache: a simulation system resolves what it needs
    // by type and cannot reach CachePrefab, UseRenderContext or the VFS handle.
    AssetLibrary            assets;
    InputReader             input;
    InputHaptics            haptics;
    ArticulationContext&    articulation;
    ECS::EventBus&          events;
    ECS::ExtensionServices& extensions;
    PoseUploadQueue&        poseUploads;
};

// What the render graph provides. `render` is here and *only* here: a
// simulation system cannot name it, which is the point.
struct RenderServices {
    RenderContext&          render;
    SceneData&              scene;
    AssetLibrary            assets;
    InputReader             input;
    ECS::ExtensionServices& extensions;
    CullingStats&           cullingStats;
    DebugSettings&          debugSettings;
    VisibleEntities         visible;
    VisibleShadowEntities   shadow;
};

namespace ECS {

// The objects a node may never name. Each has a capability that fronts it, so a
// graph that withholds the capability cannot be worked around by asking the
// extension store for the object: `Res<T>` for any of these is rejected where
// the node is added, not at run time.
template <> struct IsWithheldService<AssetManager> : std::true_type {};
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
