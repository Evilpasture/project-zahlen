// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/glTF/glTF.cpp
//
// Babylon.js-style glTF inspector:
//   - Left-side SCENE EXPLORER tree: Scene -> Models -> [glb] -> Nodes /
//     Skeletons / Materials / Textures / Animation Groups.
//   - Expand/collapse rows, selectable items with a details box, and
//     click-to-play Animation Groups driven through AnimatorComponent.
//   - Drag-and-drop loading, orbit/pan/zoom camera (input is gated while the
//     cursor is inside the explorer panel so clicks don't move the camera).

module;

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Input/InputContext.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/GUI/GUI.hpp>

// The importer lives beside this file. Note the two spellings: ZHLN::GLTF is
// the importer's namespace, ZHLN::glTF (below) is this module's.
#include "GLTFImporter.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

module ZHLN.glTF;

namespace {

using ZHLN::FVec3;
using ZHLN::FVec4;

// ============================================================================
// STATE & THEME
// ============================================================================

constexpr float kExplorerWidth   = 380.0f;
constexpr float kExplorerPadding = 10.0f;

constexpr size_t kMaxDetailLen = 240; // Keep selection details within reasonable length.

struct InspectorState {
    ZHLN::Engine*                 engine = nullptr;
    ZHLN::ModelPrefab*            prefab = nullptr;
    std::vector<ZHLN::Entity>     instances {};
    std::optional<ZHLN::FileDrop> pendingDrop {};

    FVec3 target   = {0.0f, 0.0f, 0.0f};
    float     distance = 5.0f;
    float     yaw      = -90.0f;
    float     pitch    = -15.0f;
    bool      loaded   = false;

    // ---- Scene explorer ----
    std::string                     modelName {};       // display name of the loaded asset
    std::unordered_set<std::string> expanded {};        // expanded tree paths
    std::string                     selectedPath {};    // last clicked row path
    std::string                     selectedTitle {};   // details box header
    std::string                     selectedDetails {}; // details box body
    int32_t                         playingClip = -1;   // index into prefab.animations, -1 = stopped
    ZHLN::GUI::Context              gui {};
};

[[nodiscard]] FVec4 ThemeText() noexcept {
    return FVec4{0.86f, 0.90f, 0.96f, 1.0f};
}
[[nodiscard]] FVec4 ThemeTextDim() noexcept {
    return FVec4{0.55f, 0.62f, 0.72f, 1.0f};
}
[[nodiscard]] FVec4 ThemeAccent() noexcept {
    return FVec4{0.30f, 0.85f, 1.0f, 1.0f};
}
[[nodiscard]] FVec4 ThemePlaying() noexcept {
    return FVec4{0.45f, 0.95f, 0.55f, 1.0f};
}

// ============================================================================
// CAMERA / SCENE HELPERS
// ============================================================================

[[nodiscard]] FVec3 OrbitDirection(float yawDeg, float pitchDeg) noexcept {
    const float yaw   = JPH::DegreesToRadians(yawDeg);
    const float pitch = JPH::DegreesToRadians(pitchDeg);
    return FVec3{std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)};
}

[[nodiscard]] FVec3 Cross(FVec3 a, FVec3 b) noexcept {
    return FVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

void AddInspectorLighting(ZHLN::Engine& engine) {
    auto entities = engine.Entities();

    const FVec3  sunPos   = {15.0f, 30.0f, 15.0f};
    const auto sunRot = ZHLN::Quat::FromEulerDegrees({{50.0f, -35.0f, 0.0f}});
    const auto sunWorld = ZHLN::ComposeTransform(sunPos, sunRot, FVec3::One());
    entities.Spawn(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("glTFInspectorSun")},
        ZHLN::Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = FVec3::Splat(1.0f)},
        ZHLN::Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = FVec3{1.0f, 0.97f, 0.91f}, .intensity = 120.0f, .direction = FVec3{0.4f, 1.0f, 0.3f}.Normalized()
        }
    );

    const ZHLN::Entity ground = ZHLN::PrefabFactory::CreatePlane(
        engine, 40.0f, {0.12f, 0.14f, 0.18f, 1.0f}, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .roughness = 0.9f, .metallic = 0.0f}
    );
    entities.Attach(ground, ZHLN::Components::NameComponent {.name = ZHLN::String64("glTFInspectorGround")});

    engine.EnsureFontAtlas();
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, FVec3& outCenter, float& outDistance) {
    FVec3 min{1e9f, 1e9f, 1e9f};
    FVec3 max{-1e9f, -1e9f, -1e9f};
    bool      any = false;
    for (const ZHLN::ModelPart& part: prefab.parts) {
        min.x = std::min(min.x, part.localMin[0]);
        min.y = std::min(min.y, part.localMin[1]);
        min.z = std::min(min.z, part.localMin[2]);
        max.x = std::max(max.x, part.localMax[0]);
        max.y = std::max(max.y, part.localMax[1]);
        max.z = std::max(max.z, part.localMax[2]);
        any = true;
    }
    if (!any) {
        outCenter   = FVec3::Zero();
        outDistance = 5.0f;
        return;
    }

    const FVec3 center = (min + max) * 0.5f;
    const FVec3 ext    = (max - min) * 0.5f;
    const float     radius = ext.Length();
    const float     fov    = 45.0f;
    outCenter              = center;
    outDistance            = (radius > 1e-3f) ? (radius / std::tan(JPH::DegreesToRadians(fov) * 0.5f)) * 1.4f : 5.0f;
}

void ClearInstances(InspectorState& state) {
    if (state.engine == nullptr) {
        return;
    }
    auto entities = state.engine->Entities();
    for (ZHLN::Entity e: state.instances) {
        if (e != ZHLN::Entity::Null() && entities.IsAlive(e)) {
            DespawnEntity(*state.engine, e);
        }
    }
    state.instances.clear();
}

[[nodiscard]] std::string ClampDetail(std::string text) {
    if (text.size() > kMaxDetailLen) {
        text.resize(kMaxDetailLen - 3);
        text += "...";
    }
    return text;
}

// ============================================================================
// SCENE EXPLORER TREE
// ============================================================================

// Starts/stops an animation clip on the instantiated prefab root.
void ToggleClipPlayback(InspectorState& state, int32_t clipIndex) {
    if ((state.engine == nullptr) || state.instances.empty()) {
        return;
    }
    auto               entities = state.engine->Entities();
    const ZHLN::Entity root     = state.instances[0];
    if (root == ZHLN::Entity::Null() || !entities.IsAlive(root)) {
        return;
    }
    if (!entities.Get<ZHLN::Components::AnimatorComponent>(root)) {
        return;
    }

    const bool stopping = (state.playingClip == clipIndex);
    entities.Patch<ZHLN::Components::AnimatorComponent>(root, [&](auto& anim) -> auto {
        anim.currentTrackIdx      = stopping ? -1 : clipIndex;
        anim.currentTrackTime     = 0.0f;
        anim.currentPlaybackSpeed = 1.0f;
        anim.currentLoop          = !stopping;
        anim.blendDuration        = 0.15f;
        anim.isFinished           = false;
    });
    state.playingClip = stopping ? -1 : clipIndex;
}

// Draws one indented, selectable tree row via Clay Button.
void TreeRow(
    ZHLN::GUI::Frame& ui,
    InspectorState&     state,
    std::string_view    path,
    int                 depth,
    std::string_view    label,
    bool                expandable,
    std::string_view    selectTitle,
    std::string_view    selectDetails,
    const FVec4& /*textColor*/,
    int32_t clipIndex = -1
) {
    const std::string pathStr(path);
    const bool        expanded = state.expanded.contains(pathStr);
    const bool        selected = (state.selectedPath == path);
    const bool        playing  = (clipIndex >= 0 && state.playingClip == clipIndex);

    std::string text(static_cast<size_t>(depth) * 2, ' ');
    text += expandable ? (expanded ? "v " : "> ") : "  ";
    text += label;

    FVec4 btnColor = selected ? FVec4{0.18f, 0.36f, 0.56f, 0.98f} :
                                    (playing ? FVec4{0.15f, 0.30f, 0.20f, 0.85f} : FVec4{0.10f, 0.12f, 0.16f, 0.60f});

    if (ui.Button(text, btnColor)) {
        if (expandable) {
            if (state.expanded.contains(pathStr)) {
                state.expanded.erase(pathStr);
            } else {
                state.expanded.insert(pathStr);
            }
        }
        if (clipIndex >= 0) {
            ToggleClipPlayback(state, clipIndex);
        }
        state.selectedPath    = pathStr;
        state.selectedTitle   = std::string(selectTitle);
        state.selectedDetails = std::string(selectDetails);
    }
}

void DrawModelNodeRows(
    ZHLN::GUI::Frame&        ui,
    InspectorState&          state,
    const ZHLN::ModelPrefab& prefab,
    std::string_view         basePath,
    int32_t                  nodeIndex,
    int                      depth
) {
    const ZHLN::ModelNode& node = prefab.nodes[static_cast<size_t>(nodeIndex)];
    const std::string      path = std::format("{}/n{}", basePath, nodeIndex);

    bool hasChildren = false;
    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
        if (prefab.nodes[i].parentIndex == nodeIndex) {
            hasChildren = true;
            break;
        }
    }

    std::string label = (node.name.size() > 0) ? node.name.c_str() : "<unnamed>";
    if (node.hasMesh) {
        label += "  [mesh]";
    }

    const auto& m = node.localTransform;
    const FVec3 pos {m[0, 3], m[1, 3], m[2, 3]};
    const bool      expanded = state.expanded.contains(path);
    TreeRow(
        ui, state, path, depth, label, hasChildren, std::format("Node: {}", label),
        std::format(
            "Index: {}\nParent index: {}\nMesh: {}\nPosition: ({:.3f}, {:.3f}, {:.3f})", nodeIndex, node.parentIndex, node.hasMesh ? "yes" : "no", pos.x,
            pos.y, pos.z
        ),
        ThemeText()
    );

    if (expanded && hasChildren) {
        for (size_t i = 0; i < prefab.nodes.size(); ++i) {
            if (prefab.nodes[i].parentIndex == nodeIndex) {
                DrawModelNodeRows(ui, state, prefab, path, static_cast<int32_t>(i), depth + 1);
            }
        }
    }
}

[[nodiscard]] bool SameMaterial(const ZHLN::Material& a, const ZHLN::Material& b) noexcept {
    const bool sameMaps    = (a.albedoMap == b.albedoMap) && (a.normalMap == b.normalMap) && (a.pbrMap == b.pbrMap) && (a.emissiveMap == b.emissiveMap);
    const bool sameFactors = (a.baseColorFactor == b.baseColorFactor) && (a.emissiveFactor == b.emissiveFactor);
    return sameMaps && sameFactors && (a.unlit == b.unlit) && (a.metallicFactor == b.metallicFactor) && (a.roughnessFactor == b.roughnessFactor) &&
           (a.alphaMode == b.alphaMode) && (a.alphaCutoff == b.alphaCutoff) && (a.transmissionFactor == b.transmissionFactor) &&
           (a.transmissionMap == b.transmissionMap) && (a.iridescenceFactor == b.iridescenceFactor) && (a.filmThicknessNm == b.filmThicknessNm) &&
           (a.filmThicknessMinNm == b.filmThicknessMinNm) && (a.volumeThicknessM == b.volumeThicknessM) && (a.ior == b.ior) &&
           (a.normalScale == b.normalScale) && (a.filmThicknessMap == b.filmThicknessMap) && (a.iridescenceMap == b.iridescenceMap) &&
           (a.volumeThicknessMap == b.volumeThicknessMap) && (a.clearcoatFactor == b.clearcoatFactor) &&
           (a.clearcoatRoughnessFactor == b.clearcoatRoughnessFactor) && (a.clearcoatNormalScale == b.clearcoatNormalScale) &&
           (a.clearcoatMap == b.clearcoatMap) && (a.clearcoatRoughnessMap == b.clearcoatRoughnessMap) && (a.clearcoatNormalMap == b.clearcoatNormalMap) &&
           (a.anisotropyStrength == b.anisotropyStrength) && (a.anisotropyRotation == b.anisotropyRotation) && (a.anisotropyMap == b.anisotropyMap) &&
           (a.sheenColorFactor == b.sheenColorFactor) && (a.sheenRoughnessFactor == b.sheenRoughnessFactor) && (a.sheenColorMap == b.sheenColorMap) &&
           (a.sheenRoughnessMap == b.sheenRoughnessMap) && (a.occlusionMap == b.occlusionMap) && (a.occlusionStrength == b.occlusionStrength) &&
           (a.specularEnabled == b.specularEnabled) && (a.specularFactor == b.specularFactor) && (a.specularColorFactor == b.specularColorFactor) &&
           (a.specularMap == b.specularMap) && (a.specularTexMode == b.specularTexMode) &&
           (a.textureSamplers == b.textureSamplers) && (a.textureTransforms == b.textureTransforms);
}

[[nodiscard]] std::string FormatTextureSlot(const char* slot, ZHLN::TextureID handle) {
    return std::format("{}: {}", slot, (handle != ZHLN::TextureID::Invalid) ? std::format("#{}", static_cast<uint64_t>(handle)) : std::string("-"));
}

void DrawModelContentRows(ZHLN::GUI::Frame& ui, InspectorState& state, const ZHLN::ModelPrefab& prefab, std::string_view modelPath, int depth) {
    // ---- NODES ----
    const std::string nodesPath = std::format("{}/nodes", modelPath);
    const bool        nodesOpen = state.expanded.contains(nodesPath);
    TreeRow(
        ui, state, nodesPath, depth, std::format("Nodes ({})", prefab.nodes.size()), !prefab.nodes.empty(), "Nodes",
        std::format("{} node(s) in the file hierarchy.", prefab.nodes.size()), ThemeAccent()
    );
    if (nodesOpen) {
        for (size_t i = 0; i < prefab.nodes.size(); ++i) {
            if (prefab.nodes[i].parentIndex < 0) {
                DrawModelNodeRows(ui, state, prefab, nodesPath, static_cast<int32_t>(i), depth + 1);
            }
        }
    }

    // ---- SKELETONS ----
    const std::string skeletonsPath = std::format("{}/skeletons", modelPath);
    const bool        skeletonsOpen = state.expanded.contains(skeletonsPath);
    TreeRow(
        ui, state, skeletonsPath, depth, std::format("Skeletons ({})", prefab.skeletons.size()), !prefab.skeletons.empty(), "Skeletons",
        std::format("{} skinned rig(s).", prefab.skeletons.size()), ThemeAccent()
    );
    if (skeletonsOpen) {
        for (size_t s = 0; s < prefab.skeletons.size(); ++s) {
            const ZHLN::Skeleton& skel  = prefab.skeletons[s];
            const std::string     sPath = std::format("{}/s{}", skeletonsPath, s);
            const std::string     sName = (skel.name.size() > 0) ? skel.name.c_str() : std::format("Skeleton {}", s);
            const bool            sOpen = state.expanded.contains(sPath);
            TreeRow(
                ui, state, sPath, depth + 1, std::format("{}  ({} joints)", sName, skel.joints.size()), !skel.joints.empty(),
                std::format("Skeleton: {}", sName), std::format("Joints: {}", skel.joints.size()), ThemeText()
            );
            if (sOpen) {
                for (size_t j = 0; j < skel.joints.size(); ++j) {
                    const ZHLN::Joint& joint = skel.joints[j];
                    int                d     = 0;
                    int32_t            p     = joint.parentIndex;
                    int                guard = 0;
                    while (p >= 0 && guard < 256) {
                        ++d;
                        p = skel.joints[static_cast<size_t>(p)].parentIndex;
                        ++guard;
                    }
                    const std::string jName = (joint.name.size() > 0) ? joint.name.c_str() : std::format("Joint {}", j);
                    TreeRow(
                        ui, state, std::format("{}/j{}", sPath, j), depth + 2 + d, jName, false, std::format("Joint: {}", jName),
                        std::format("Skeleton: {}\nJoint index: {}\nNode index: {}", sName, j, joint.nodeIndex), ThemeTextDim()
                    );
                }
            }
        }
    }

    // ---- MATERIALS (deduplicated across parts) ----
    std::vector<size_t> uniqueMaterials;
    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        bool seen = false;
        for (size_t u: uniqueMaterials) {
            if (SameMaterial(prefab.parts[i].defaultMaterial, prefab.parts[u].defaultMaterial)) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            uniqueMaterials.push_back(i);
        }
    }

    const std::string materialsPath = std::format("{}/materials", modelPath);
    const bool        materialsOpen = state.expanded.contains(materialsPath);
    TreeRow(
        ui, state, materialsPath, depth, std::format("Materials ({})", uniqueMaterials.size()), !uniqueMaterials.empty(), "Materials",
        std::format("{} unique material(s) across {} part(s).", uniqueMaterials.size(), prefab.parts.size()), ThemeAccent()
    );
    if (materialsOpen) {
        for (size_t m = 0; m < uniqueMaterials.size(); ++m) {
            const size_t           partIdx = uniqueMaterials[m];
            const ZHLN::ModelPart& part    = prefab.parts[partIdx];
            const ZHLN::Material&  mat     = part.defaultMaterial;

            size_t sharedBy = 0;
            for (const ZHLN::ModelPart& other: prefab.parts) {
                if (SameMaterial(other.defaultMaterial, mat)) {
                    ++sharedBy;
                }
            }

            const std::string pName = (part.name.size() > 0) ? part.name.c_str() : std::format("Part {}", partIdx);
            std::string       label = pName;
            if (sharedBy > 1) {
                label += std::format("  (x{})", sharedBy);
            }

            TreeRow(
                ui, state, std::format("{}/m{}", materialsPath, m), depth + 1, label, false, std::format("Material: {}", label),
                ClampDetail(
                    std::format(
                        "Part: {}\nBaseColor: ({:.2f}, {:.2f}, {:.2f}, {:.2f})\nMetallic: {:.2f}  Roughness: {:.2f}\nAlphaMode: {}  Cutoff: "
                        "{:.2f}\n{}\n{}\n{}\n{}",
                        pName, mat.baseColorFactor[0], mat.baseColorFactor[1], mat.baseColorFactor[2], mat.baseColorFactor[3], mat.metallicFactor,
                        mat.roughnessFactor, mat.alphaMode, mat.alphaCutoff, FormatTextureSlot("Albedo", mat.albedoMap),
                        FormatTextureSlot("Normal", mat.normalMap), FormatTextureSlot("PBR", mat.pbrMap), FormatTextureSlot("Emissive", mat.emissiveMap)
                    )
                ),
                ThemeText()
            );
        }
    }

    // ---- TEXTURES (unique texture IDs across all material slots) ----
    struct TextureEntry {
        ZHLN::TextureID textureID;
        std::string         roles;
        std::string         parts;
    };
    std::vector<TextureEntry> textures;
    auto                      addTextureSlot = [&](ZHLN::TextureID handle, const char* role, std::string_view partName) {
        if (handle == ZHLN::TextureID::Invalid) {
            return;
        }
        for (auto& entry: textures) {
            if (entry.textureID == handle) {
                if (entry.roles.find(role) == std::string::npos) {
                    entry.roles += ", ";
                    entry.roles += role;
                }
                if (entry.parts.find(partName) == std::string::npos) {
                    entry.parts += ", ";
                    entry.parts += partName;
                }
                return;
            }
        }
        textures.push_back(TextureEntry {.textureID = handle, .roles = role, .parts = std::string(partName)});
    };
    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        const ZHLN::Material& mat   = prefab.parts[i].defaultMaterial;
        const std::string     pName = (prefab.parts[i].name.size() > 0) ? prefab.parts[i].name.c_str() : std::format("Part {}", i);
        addTextureSlot(mat.albedoMap, "albedo", pName);
        addTextureSlot(mat.normalMap, "normal", pName);
        addTextureSlot(mat.pbrMap, "pbr", pName);
        addTextureSlot(mat.emissiveMap, "emissive", pName);
        addTextureSlot(mat.transmissionMap, "transmission", pName);
    }

    const std::string texturesPath = std::format("{}/textures", modelPath);
    const bool        texturesOpen = state.expanded.contains(texturesPath);
    TreeRow(
        ui, state, texturesPath, depth, std::format("Textures ({})", textures.size()), !textures.empty(), "Textures",
        std::format("{} unique texture(s) referenced by materials.", textures.size()), ThemeAccent()
    );
    if (texturesOpen) {
        for (size_t t = 0; t < textures.size(); ++t) {
            const TextureEntry& entry = textures[t];
            TreeRow(
                ui, state, std::format("{}/t{}", texturesPath, t), depth + 1, std::format("#{}  ({})", static_cast<uint64_t>(entry.textureID), entry.roles), false,
                std::format("Texture #{}", static_cast<uint64_t>(entry.textureID)),
                ClampDetail(std::format("Texture ID: #{}\nRole(s): {}\nUsed by: {}", static_cast<uint64_t>(entry.textureID), entry.roles, entry.parts)), ThemeText()
            );
        }
    }

    // ---- ANIMATION GROUPS ----
    const std::string animsPath = std::format("{}/animations", modelPath);
    const bool        animsOpen = state.expanded.contains(animsPath);
    TreeRow(
        ui, state, animsPath, depth, std::format("Animation Groups ({})", prefab.animations.size()), !prefab.animations.empty(), "Animation Groups",
        std::format("{} clip(s). Click a clip to play / stop it.", prefab.animations.size()), ThemeAccent()
    );
    if (animsOpen) {
        for (size_t c = 0; c < prefab.animations.size(); ++c) {
            const ZHLN::AnimationClip& clip     = prefab.animations[c];
            const std::string          clipName = (clip.name.size() > 0) ? clip.name.c_str() : std::format("Clip {}", c);
            const bool                 playing  = (state.playingClip == static_cast<int32_t>(c));
            std::string                label    = std::format("{}  ({:.2f}s, {} ch)", clipName, clip.duration, clip.channels.size());
            if (playing) {
                label += "  [playing]";
            }
            TreeRow(
                ui, state, std::format("{}/c{}", animsPath, c), depth + 1, label, false, std::format("Animation Group: {}", clipName),
                std::format(
                    "Duration: {:.3f}s\nChannels: {}\nState: {}\nClick the row to {}.", clip.duration, clip.channels.size(), playing ? "playing" : "stopped",
                    playing ? "stop" : "play"
                ),
                playing ? ThemePlaying() : ThemeText(), static_cast<int32_t>(c)
            );
        }
    }

    // ---- LIGHTS ----
    const std::string lightsPath = std::format("{}/lights", modelPath);
    const bool        lightsOpen = state.expanded.contains(lightsPath);
    TreeRow(
        ui, state, lightsPath, depth, std::format("Lights ({})", prefab.lights.size()), !prefab.lights.empty(), "Lights",
        std::format("{} punctual light(s) defined in model.", prefab.lights.size()), ThemeAccent()
    );

    if (lightsOpen) {
        for (size_t i = 0; i < prefab.lights.size(); ++i) {
            const auto&       light = prefab.lights[i];
            const std::string lPath = std::format("{}/l{}", lightsPath, i);
            const std::string lName = (light.name.size() > 0) ? light.name.c_str() : std::format("Light {}", i);

            TreeRow(
                ui, state, lPath, depth + 1, lName, false, std::format("Light: {}", lName),
                std::format(
                    "Type: {}\nColor: ({:.2f}, {:.2f}, {:.2f})\nIntensity: {:.1f}\nRange: {:.1f}", static_cast<int>(light.type), light.color.x, light.color.y,
                    light.color.z, light.intensity, light.range
                ),
                ThemeText()
            );
        }
    }
}

void DrawSceneExplorer(ZHLN::GUI::Frame& ui, InspectorState& state) {
    ui.Box(
        "glTFSceneExplorer",
        ZHLN::GUI::BoxConfig {
            .width        = {.fixed = kExplorerWidth},
            .height       = {.grow = 1.0f},
            .color        = {0.07f, 0.09f, 0.13f, 0.96f},
            .cornerRadius = {0.0f, 0.0f, 0.0f, 0.0f},
            .padding      = kExplorerPadding,
            .gap          = 4.0f,
            .direction    = ZHLN::GUI::Direction::Column
        },
        [&]() -> void {
            ui.Text("SCENE EXPLORER", 16.0f, {0.30f, 0.85f, 1.0f, 1.0f});

            const ZHLN::ModelPrefab* prefab     = (state.loaded && state.prefab != nullptr) ? state.prefab : nullptr;
            const size_t             modelCount = (prefab != nullptr) ? 1u : 0u;

            ui.Text((prefab != nullptr) ? state.modelName : "no model loaded", 12.0f, {0.55f, 0.62f, 0.72f, 1.0f});

            // ---- SCENE ROOT ----
            const bool sceneOpen = state.expanded.contains("scene");
            TreeRow(
                ui, state, "scene", 0, "SCENE", true, "Scene",
                std::format("Zahlen glTF Inspector\nModels: {}\nInstances: {}", modelCount, state.instances.size()), ThemeAccent()
            );

            if (sceneOpen) {
                const bool modelsOpen = state.expanded.contains("scene/models");
                TreeRow(
                    ui, state, "scene/models", 1, std::format("Models ({})", modelCount), true, "Models",
                    "Loaded glTF assets.\nEach dropped .glb / .gltf becomes one model here.", ThemeAccent()
                );

                if (modelsOpen) {
                    if (prefab == nullptr) {
                        ui.Text("      (drop a .glb / .gltf onto the window)", 12.0f, {0.45f, 0.50f, 0.58f, 1.0f});
                    } else {
                        const std::string modelPath = "scene/models/" + state.modelName;
                        TreeRow(
                            ui, state, modelPath, 2, state.modelName, true, std::format("Model: {}", state.modelName),
                            std::format(
                                "File: {}\nParts: {}  Nodes: {}\nSkeletons: {}  Animations: {}", state.modelName, prefab->parts.size(), prefab->nodes.size(),
                                prefab->skeletons.size(), prefab->animations.size()
                            ),
                            ThemeText()
                        );

                        if (state.expanded.contains(modelPath)) {
                            DrawModelContentRows(ui, state, *prefab, modelPath, 3);
                        }
                    }
                }
            }

            // ---- SELECTION DETAILS ----
            ui.Box(
                "SelectionDetailsBox",
                ZHLN::GUI::BoxConfig {
                    .width        = {.grow = 1.0f},
                    .height       = {.fixed = 178.0f},
                    .color        = {0.05f, 0.07f, 0.11f, 0.85f},
                    .cornerRadius = {4.0f, 4.0f, 4.0f, 4.0f},
                    .padding      = 10.0f,
                    .gap          = 4.0f,
                    .direction    = ZHLN::GUI::Direction::Column
                },
                [&]() -> void {
                    ui.Text(state.selectedTitle.empty() ? "Selection" : state.selectedTitle, 14.0f, {0.30f, 0.85f, 1.0f, 1.0f});
                    ui.Text(
                        state.selectedDetails.empty() ? "Click an item in the tree to inspect it." : state.selectedDetails, 12.0f, {0.72f, 0.78f, 0.86f, 1.0f}
                    );
                }
            );

            ui.Text("LMB drag: orbit | RMB drag: pan | Wheel: zoom", 11.0f, {0.45f, 0.50f, 0.58f, 1.0f});
        }
    );
}

// ============================================================================
// LOADING / INPUT
// ============================================================================

void LoadDroppedModel(InspectorState& state, const ZHLN::FileDrop& drop) {
    auto& engine = *state.engine;
    ClearInstances(state);

    auto prefab = ZHLN::GLTF::LoadGLBPrefabFromMemory(
        engine, std::span<const uint8_t>(drop.data.data(), drop.data.size()), drop.fileName
    );
    if (!prefab) {
        ZHLN::Log("[glTF Inspector] Failed to parse '{}' as glTF.", drop.fileName);
        return;
    }

    const uint32_t capacity = 1u + static_cast<uint32_t>(prefab->parts.size());
    state.instances.resize(static_cast<size_t>(capacity));
    const uint32_t written = ZHLN::PrefabFactory::InstantiatePrefab(
        engine, *prefab, ZHLN::PrefabFactory::SpawnParams {.position = {0.0, 0.0, 0.0}, .createPhysics = false, .isAnimated = true}, state.instances.data(),
        capacity
    );

    ComputeBounds(*prefab, state.target, state.distance);

    // Reset & seed the scene explorer: Scene -> Models -> [model] unfolded.
    state.modelName   = drop.fileName;
    state.expanded    = {"scene", "scene/models", "scene/models/" + state.modelName};
    state.playingClip = prefab->animations.empty() ? -1 : 0;

    state.selectedPath    = "scene/models/" + state.modelName;
    state.selectedTitle   = std::format("Model: {}", state.modelName);
    state.selectedDetails = std::format(
        "File: {}\nParts: {}  Nodes: {}\nSkeletons: {}  Animations: {}", state.modelName, prefab->parts.size(), prefab->nodes.size(), prefab->skeletons.size(),
        prefab->animations.size()
    );

    state.prefab = &*prefab;
    state.loaded = true;
    ZHLN::Log(
        "[glTF Inspector] Loaded '{}': {} part(s), {} node(s), {} skeleton(s), {} animation(s), {} instance(s).", drop.fileName, prefab->parts.size(),
        prefab->nodes.size(), prefab->skeletons.size(), prefab->animations.size(), written
    );
}

// Immediately stores the drop and returns so the OS event loop and Thunar aren't blocked.
void OnFileDropped(void* userdata, const ZHLN::FileDrop* files, uint32_t count) {
    auto* state = static_cast<InspectorState*>(userdata);
    if (state == nullptr || state->engine == nullptr || files == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const ZHLN::FileDrop& drop = files[i];
        if (drop.data.empty()) {
            continue;
        }
        const bool isGLTF = (drop.format == "glb") || (drop.format == "gltf");
        if (!isGLTF) {
            ZHLN::Log("[glTF Inspector] Ignoring non-glTF drop: '{}' (.{}).", drop.fileName, drop.format);
            continue;
        }
        state->pendingDrop = drop;
        break;
    }
}

void UpdateOrbit(InspectorState& state, ZHLN::Engine& engine) {
    const auto& input = engine.GetInput().SeatKeyboardMouse();

    // The scene explorer panel owns input inside its screen region (once shown).
    const bool overExplorer = state.loaded && (input.mouseX >= 0.0f) && (input.mouseX <= kExplorerWidth);
    if (overExplorer) {
        return;
    }

    const float dx  = input.mouseDeltaX;
    const float dy  = input.mouseDeltaY;
    const bool  lmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool  rmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton));
    const bool  mmb = input.IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton));

    constexpr float kOrbitSpeed = 0.30f;
    constexpr float kPanSpeed   = 0.0015f;
    constexpr float kZoomSpeed  = 0.15f;

    if (lmb || mmb) {
        state.yaw += dx * kOrbitSpeed;
        state.pitch -= dy * kOrbitSpeed;
        state.pitch = std::clamp(state.pitch, -89.0f, 89.0f);
    } else if (rmb) {
        const FVec3 forward  = OrbitDirection(state.yaw, state.pitch).Normalized();
        FVec3       camRight = Cross(forward, FVec3::UnitY());
        if (camRight.LengthSquared() > 1e-6f) {
            camRight = camRight.Normalized();
        }
        const FVec3 camUp = Cross(camRight, forward).Normalized();
        const float     scale = state.distance * kPanSpeed;

        state.target += (-camRight * dx + camUp * dy) * scale;
    }

    if (input.mouseWheel != 0.0f) {
        state.distance *= std::exp(-input.mouseWheel * kZoomSpeed);
        state.distance = std::clamp(state.distance, 0.05f, 5000.0f);
    }

    const FVec3 dir = OrbitDirection(state.yaw, state.pitch);
    engine.Entities().PatchSingleton<ZHLN::Components::CameraComponent>([&](ZHLN::Components::CameraComponent& cameraComp) {
        ZHLN::Camera& cam = cameraComp.camera;
        cam.position      = state.target - dir * state.distance;
        cam.yaw           = ZHLN::DegF{state.yaw};
        cam.pitch         = ZHLN::DegF{state.pitch};
    });
}

void DrawDropPrompt(ZHLN::GUI::Frame& ui) {
    ui.Box(
        "glTFInspectorPrompt",
        ZHLN::GUI::BoxConfig {
            .width        = {.fixed = 560.0f},
            .height       = {.fixed = 160.0f},
            .color        = {0.06f, 0.09f, 0.14f, 0.95f},
            .cornerRadius = {8.0f, 8.0f, 8.0f, 8.0f},
            .padding      = 24.0f,
            .gap          = 12.0f,
            .direction    = ZHLN::GUI::Direction::Column
        },
        [&]() -> void {
            ui.Text("glTF Inspector", 20.0f, {0.3f, 0.85f, 1.0f, 1.0f});
            ui.Text("Drop a glTF (.glb / .gltf) file onto this window to inspect it.", 14.0f, {0.85f, 0.90f, 0.95f, 1.0f});
        }
    );
}

void RenderFrame(ZHLN::Engine& engine) {
    auto* state = static_cast<InspectorState*>(engine.GetGameState());
    if (state == nullptr) {
        return;
    }

    // Heavy model loading runs on the engine frame tick, outside the OS callback.
    if (state->pendingDrop.has_value()) {
        ZHLN::FileDrop drop = std::move(*state->pendingDrop);
        state->pendingDrop.reset();
        LoadDroppedModel(*state, drop);
    }

    UpdateOrbit(*state, engine);

    state->gui.SetFontAtlas(engine.GetFontAtlas());
    ZHLN::GUI::Frame ui = state->gui.BeginFrame(engine.ConsumeUIInput(0.016667f));

    if (state->loaded) {
        DrawSceneExplorer(ui, *state);
    } else {
        DrawDropPrompt(ui);
    }

    engine.SubmitUI(std::move(ui).End());
}

} // namespace

namespace ZHLN::glTF {

void Initialize(ZHLN::Engine& engine) {
    ZHLN::GLTF::InstallDeviceLostHandler(engine);

    auto* state   = new InspectorState();
    state->engine = &engine;
    engine.SetGameState(state);

    AddInspectorLighting(engine);

    engine.GetPlatformHost().SetFileDropHandler(&OnFileDropped, state);
    engine.SetUICallback([](ZHLN::Engine& eng) -> void { RenderFrame(eng); });

    ZHLN::Log("[glTF Inspector] Initialized. Drop a .glb / .gltf file to begin.");
}

} // namespace ZHLN::glTF
