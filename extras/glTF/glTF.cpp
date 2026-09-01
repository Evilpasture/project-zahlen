// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/glTF/glTF.cpp
//
// Babylon.js-style glTF inspector:
//   - Left-side SCENE EXPLORER tree: Scene -> Models -> [glb] -> Nodes /
//     Skeletons / Materials / Textures / Animation Groups.
//   - Expand/collapse rows, selectable items with a details box, and
//     click-to-play Animation Groups driven through AnimatorComponent.
//   - Drag-and-drop loading, orbit/pan/zoom camera (input is gated while the
//     cursor is inside the explorer panel so clicks don't move the camera).

module;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
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

// ============================================================================
// STATE & THEME
// ============================================================================

constexpr float kExplorerWidth   = 380.0f;
constexpr float kExplorerPadding = 10.0f;
constexpr float kRowWidth        = kExplorerWidth - 2.0f * kExplorerPadding;
constexpr float kRowHeight       = 24.0f;

constexpr size_t kMaxDetailLen = 240; // Keep selection details within String256.

struct InspectorState {
    ZHLN::Engine*                 engine = nullptr;
    ZHLN::ModelPrefab*            prefab = nullptr;
    std::vector<ZHLN::Entity>     instances {};
    std::optional<ZHLN::FileDrop> pendingDrop {};

    JPH::Vec3 target   = JPH::Vec3(0.0f, 0.0f, 0.0f);
    float     distance = 5.0f;
    float     yaw      = -90.0f;
    float     pitch    = -15.0f;
    bool      loaded   = false;

    // ---- Scene explorer ----
    std::string                     modelName {};        // display name of the loaded asset
    std::unordered_set<std::string> expanded {};         // expanded tree paths
    std::string                     selectedPath {};     // last clicked row path
    std::string                     selectedTitle {};    // details box header
    std::string                     selectedDetails {};  // details box body
    int32_t                         playingClip = -1;    // index into prefab.animations, -1 = stopped
};

[[nodiscard]] JPH::Vec4 ThemeText() noexcept {
    return JPH::Vec4(0.86f, 0.90f, 0.96f, 1.0f);
}
[[nodiscard]] JPH::Vec4 ThemeTextDim() noexcept {
    return JPH::Vec4(0.55f, 0.62f, 0.72f, 1.0f);
}
[[nodiscard]] JPH::Vec4 ThemeAccent() noexcept {
    return JPH::Vec4(0.30f, 0.85f, 1.0f, 1.0f);
}
[[nodiscard]] JPH::Vec4 ThemePlaying() noexcept {
    return JPH::Vec4(0.45f, 0.95f, 0.55f, 1.0f);
}

// ============================================================================
// CAMERA / SCENE HELPERS
// ============================================================================

[[nodiscard]] JPH::Vec3 OrbitDirection(float yawDeg, float pitchDeg) noexcept {
    const float yaw   = JPH::DegreesToRadians(yawDeg);
    const float pitch = JPH::DegreesToRadians(pitchDeg);
    return JPH::Vec3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
}

[[nodiscard]] JPH::Vec3 Cross(JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    return JPH::Vec3(a.GetY() * b.GetZ() - a.GetZ() * b.GetY(), a.GetZ() * b.GetX() - a.GetX() * b.GetZ(), a.GetX() * b.GetY() - a.GetY() * b.GetX());
}

void AddInspectorLighting(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();

    const JPH::Vec3  sunPos   = JPH::Vec3(15.0f, 30.0f, 15.0f);
    const JPH::Quat  sunRot   = ZHLN::Math::EulerDegreesToQuat(JPH::Vec3(50.0f, -35.0f, 0.0f));
    const JPH::Mat44 sunWorld = ZHLN::Math::CreateTransform(sunPos, sunRot);
    reg.Create(
        ZHLN::Components::NameComponent {.name = ZHLN::String64("glTFInspectorSun")},
        ZHLN::Components::TransformComponent {.position = sunPos, .rotation = sunRot, .scale = JPH::Vec3::sReplicate(1.0f)},
        ZHLN::Components::WorldTransformComponent {.world = sunWorld, .previous = sunWorld},
        ZHLN::Components::LightComponent {
            .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 0.97f, 0.91f), .intensity = 120.0f, .direction = JPH::Vec3(0.4f, 1.0f, 0.3f).Normalized()
        }
    );

    const ZHLN::Entity ground = ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 40.0f, JPH::Vec4(0.12f, 0.14f, 0.18f, 1.0f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .roughness = 0.9f, .metallic = 0.0f}
    );
    reg.Assign<ZHLN::Components::NameComponent>(ground, "glTFInspectorGround");

    auto uiSettingsEnts = reg.GetEntitiesWith<ZHLN::Components::UISettingsComponent>();
    if (!uiSettingsEnts.empty()) {
        if (auto* settings = reg.Get<ZHLN::Components::UISettingsComponent>(uiSettingsEnts[0])) {
            if (settings->fontAtlas.texture == ZHLN::TextureHandle::Invalid) {
                settings->fontAtlas.texture = ZHLN::CreativeWorksFactory::CreateFontAtlasTexture(engine.GetRenderContext(), engine.GetRegistry());
                settings->defaultFontAtlas  = settings->fontAtlas.texture;
            }
        }
    }
}

void ComputeBounds(const ZHLN::ModelPrefab& prefab, JPH::Vec3& outCenter, float& outDistance) {
    JPH::Vec3 min(1e9f, 1e9f, 1e9f);
    JPH::Vec3 max(-1e9f, -1e9f, -1e9f);
    bool      any = false;
    for (const ZHLN::ModelPart& part: prefab.parts) {
        min.SetX(std::min(min.GetX(), part.localMin[0]));
        min.SetY(std::min(min.GetY(), part.localMin[1]));
        min.SetZ(std::min(min.GetZ(), part.localMin[2]));
        max.SetX(std::max(max.GetX(), part.localMax[0]));
        max.SetY(std::max(max.GetY(), part.localMax[1]));
        max.SetZ(std::max(max.GetZ(), part.localMax[2]));
        any = true;
    }
    if (!any) {
        outCenter   = JPH::Vec3::sZero();
        outDistance = 5.0f;
        return;
    }

    const JPH::Vec3 center = (min + max) * 0.5f;
    const JPH::Vec3 ext    = (max - min) * 0.5f;
    const float     radius = ext.Length();
    const float     fov    = 45.0f;
    outCenter              = center;
    outDistance            = (radius > 1e-3f) ? (radius / std::tan(JPH::DegreesToRadians(fov) * 0.5f)) * 1.4f : 5.0f;
}

void ClearInstances(InspectorState& state) {
    if (state.engine == nullptr) {
        return;
    }
    auto& reg = state.engine->GetRegistry();
    for (ZHLN::Entity e: state.instances) {
        if (e != ZHLN::Entity::Null() && reg.IsAlive(e)) {
            reg.Destroy(e);
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

// Starts/stops an animation clip on the instantiated prefab root. Clicking the
// clip that is already playing stops it (returns the rig to its base pose).
void ToggleClipPlayback(InspectorState& state, int32_t clipIndex) {
    if ((state.engine == nullptr) || state.instances.empty()) {
        return;
    }
    auto&              reg  = state.engine->GetRegistry();
    const ZHLN::Entity root = state.instances[0];
    if (root == ZHLN::Entity::Null() || !reg.IsAlive(root)) {
        return;
    }
    if (reg.Get<ZHLN::Components::AnimatorComponent>(root) == nullptr) {
        return;
    }

    const bool stopping = (state.playingClip == clipIndex);
    reg.Patch<ZHLN::Components::AnimatorComponent>(root, [&](auto& anim) -> auto {
        anim.prevTrackIdx      = anim.currentTrackIdx;
        anim.prevTrackTime     = anim.currentTrackTime;
        anim.prevPlaybackSpeed = anim.currentPlaybackSpeed;

        anim.currentTrackIdx      = stopping ? -1 : clipIndex;
        anim.currentTrackTime     = 0.0f;
        anim.currentPlaybackSpeed = 1.0f;
        anim.currentLoop          = !stopping;
        anim.blendFactor          = 0.0f;
        anim.blendDuration        = 0.15f;
        anim.isFinished           = false;
    });
    state.playingClip = stopping ? -1 : clipIndex;
}

// Draws one indented, selectable tree row. Expandable rows toggle their
// expanded state on click. Clip rows (clipIndex >= 0) also toggle playback.
void TreeRow(
    ZHLN::GUI::Context& ui,
    InspectorState&     state,
    const std::string&  path,
    int                 depth,
    const std::string&  label,
    bool                expandable,
    const std::string&  selectTitle,
    const std::string&  selectDetails,
    const JPH::Vec4&    textColor,
    int32_t             clipIndex = -1
) {
    const bool expanded = state.expanded.contains(path);
    const bool selected = (state.selectedPath == path);

    std::string text(static_cast<size_t>(depth) * 2, ' ');
    text += expandable ? (expanded ? "v " : "> ") : "  ";
    text += label;

    const JPH::Vec4 normalColor  = selected ? JPH::Vec4(0.18f, 0.36f, 0.56f, 0.98f) : JPH::Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    const JPH::Vec4 hoverColor   = selected ? JPH::Vec4(0.26f, 0.46f, 0.68f, 1.0f) : JPH::Vec4(0.16f, 0.24f, 0.36f, 0.85f);
    const JPH::Vec4 pressedColor = JPH::Vec4(0.10f, 0.16f, 0.26f, 0.95f);

    ZHLN::Entity row = ui.Button(
        path,
        text,
        ZHLN::GUI::ButtonConfig {
            .width         = kRowWidth,
            .height        = kRowHeight,
            .scale         = 0.72f,
            .normalColor   = normalColor,
            .hoverColor    = hoverColor,
            .pressedColor  = pressedColor,
            .textColor     = textColor,
            .borderRadius  = {3.0f, 3.0f, 3.0f, 3.0f},
            .align         = ZHLN::TextAlignment::Left,
            .verticalAlign = ZHLN::TextVerticalAlignment::Center,
        },
        [&]() -> void {
            if (expandable) {
                if (state.expanded.contains(path)) {
                    state.expanded.erase(path);
                } else {
                    state.expanded.insert(path);
                }
            }
            if (clipIndex >= 0) {
                ToggleClipPlayback(state, clipIndex);
            }
            state.selectedPath    = path;
            state.selectedTitle   = selectTitle;
            state.selectedDetails = selectDetails;
        }
    );

    // UIInteractionSystem lerps panel/text colors from UIStyleComponent every
    // frame; refresh it so selection & playback tints apply immediately.
    auto& reg = ui.GetRegistry();
    reg.Patch<ZHLN::Components::UIStyleComponent>(row, [&](auto& style) -> auto {
        style.normalColor     = normalColor;
        style.hoverColor      = hoverColor;
        style.pressedColor    = pressedColor;
        style.textColorNormal = textColor;
        style.transitionSpeed = 0.0f;
    });
}

void DrawModelNodeRows(
    ZHLN::GUI::Context&      ui,
    InspectorState&          state,
    const ZHLN::ModelPrefab& prefab,
    const std::string&       basePath,
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

    const JPH::Vec3 pos     = node.localTransform.GetTranslation();
    const bool      expanded = state.expanded.contains(path);
    TreeRow(
        ui, state, path, depth, label, hasChildren, std::format("Node: {}", label),
        std::format("Index: {}\nParent index: {}\nMesh: {}\nPosition: ({:.3f}, {:.3f}, {:.3f})", nodeIndex, node.parentIndex,
                    node.hasMesh ? "yes" : "no", pos.GetX(), pos.GetY(), pos.GetZ()),
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
    const bool sameMaps = (a.albedoMap == b.albedoMap) && (a.normalMap == b.normalMap) && (a.pbrMap == b.pbrMap) && (a.emissiveMap == b.emissiveMap);
    const bool sameFactors = std::equal(std::begin(a.baseColorFactor), std::end(a.baseColorFactor), std::begin(b.baseColorFactor)) &&
                             std::equal(std::begin(a.emissiveFactor), std::end(a.emissiveFactor), std::begin(b.emissiveFactor));
    return sameMaps && sameFactors && (a.metallicFactor == b.metallicFactor) && (a.roughnessFactor == b.roughnessFactor) &&
           (a.alphaMode == b.alphaMode) && (a.alphaCutoff == b.alphaCutoff);
}

[[nodiscard]] std::string FormatTextureSlot(const char* slot, ZHLN::TextureHandle handle) {
    return std::format("{}: {}", slot, (handle != ZHLN::TextureHandle::Invalid) ? std::format("#{}", static_cast<uint64_t>(handle)) : std::string("-"));
}

void DrawModelContentRows(ZHLN::GUI::Context& ui, InspectorState& state, const ZHLN::ModelPrefab& prefab, const std::string& modelPath, int depth) {
    // ---- NODES ----
    const std::string nodesPath = modelPath + "/nodes";
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
    const std::string skeletonsPath = modelPath + "/skeletons";
    const bool        skeletonsOpen = state.expanded.contains(skeletonsPath);
    TreeRow(
        ui, state, skeletonsPath, depth, std::format("Skeletons ({})", prefab.skeletons.size()), !prefab.skeletons.empty(), "Skeletons",
        std::format("{} skinned rig(s).", prefab.skeletons.size()), ThemeAccent()
    );
    if (skeletonsOpen) {
        for (size_t s = 0; s < prefab.skeletons.size(); ++s) {
            const ZHLN::Skeleton& skel   = prefab.skeletons[s];
            const std::string     sPath  = std::format("{}/s{}", skeletonsPath, s);
            const std::string     sName  = (skel.name.size() > 0) ? skel.name.c_str() : std::format("Skeleton {}", s);
            const bool            sOpen  = state.expanded.contains(sPath);
            TreeRow(
                ui, state, sPath, depth + 1, std::format("{}  ({} joints)", sName, skel.joints.size()), !skel.joints.empty(),
                std::format("Skeleton: {}", sName), std::format("Joints: {}", skel.joints.size()), ThemeText()
            );
            if (sOpen) {
                for (size_t j = 0; j < skel.joints.size(); ++j) {
                    const ZHLN::Joint& joint = skel.joints[j];
                    // Joint depth inside the skeleton hierarchy (guarded walk).
                    int     d     = 0;
                    int32_t p     = joint.parentIndex;
                    int     guard = 0;
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

    const std::string materialsPath = modelPath + "/materials";
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
                ClampDetail(std::format(
                    "Part: {}\nBaseColor: ({:.2f}, {:.2f}, {:.2f}, {:.2f})\nMetallic: {:.2f}  Roughness: {:.2f}\nAlphaMode: {}  Cutoff: {:.2f}\n{}\n{}\n{}\n{}",
                    pName, mat.baseColorFactor[0], mat.baseColorFactor[1], mat.baseColorFactor[2], mat.baseColorFactor[3], mat.metallicFactor,
                    mat.roughnessFactor, mat.alphaMode, mat.alphaCutoff, FormatTextureSlot("Albedo", mat.albedoMap),
                    FormatTextureSlot("Normal", mat.normalMap), FormatTextureSlot("PBR", mat.pbrMap), FormatTextureSlot("Emissive", mat.emissiveMap)
                )),
                ThemeText()
            );
        }
    }

    // ---- TEXTURES (unique handles across all material slots) ----
    struct TextureEntry {
        ZHLN::TextureHandle handle;
        std::string         roles;
        std::string         parts;
    };
    std::vector<TextureEntry> textures;
    auto                      addTextureSlot = [&](ZHLN::TextureHandle handle, const char* role, const std::string& partName) {
        if (handle == ZHLN::TextureHandle::Invalid) {
            return;
        }
        for (auto& entry: textures) {
            if (entry.handle == handle) {
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
        textures.push_back(TextureEntry {.handle = handle, .roles = role, .parts = partName});
    };
    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        const ZHLN::Material& mat     = prefab.parts[i].defaultMaterial;
        const std::string     pName   = (prefab.parts[i].name.size() > 0) ? prefab.parts[i].name.c_str() : std::format("Part {}", i);
        addTextureSlot(mat.albedoMap, "albedo", pName);
        addTextureSlot(mat.normalMap, "normal", pName);
        addTextureSlot(mat.pbrMap, "pbr", pName);
        addTextureSlot(mat.emissiveMap, "emissive", pName);
    }

    const std::string texturesPath = modelPath + "/textures";
    const bool        texturesOpen = state.expanded.contains(texturesPath);
    TreeRow(
        ui, state, texturesPath, depth, std::format("Textures ({})", textures.size()), !textures.empty(), "Textures",
        std::format("{} unique texture(s) referenced by materials.", textures.size()), ThemeAccent()
    );
    if (texturesOpen) {
        for (size_t t = 0; t < textures.size(); ++t) {
            const TextureEntry& entry = textures[t];
            TreeRow(
                ui, state, std::format("{}/t{}", texturesPath, t), depth + 1,
                std::format("#{}  ({})", static_cast<uint64_t>(entry.handle), entry.roles), false,
                std::format("Texture #{}", static_cast<uint64_t>(entry.handle)),
                ClampDetail(std::format("Handle: #{}\nRole(s): {}\nUsed by: {}", static_cast<uint64_t>(entry.handle), entry.roles, entry.parts)),
                ThemeText()
            );
        }
    }

    // ---- ANIMATION GROUPS ----
    const std::string animsPath = modelPath + "/animations";
    const bool        animsOpen = state.expanded.contains(animsPath);
    TreeRow(
        ui, state, animsPath, depth, std::format("Animation Groups ({})", prefab.animations.size()), !prefab.animations.empty(),
        "Animation Groups", std::format("{} clip(s). Click a clip to play / stop it.", prefab.animations.size()), ThemeAccent()
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
                    "Duration: {:.3f}s\nChannels: {}\nState: {}\nClick the row to {}.", clip.duration, clip.channels.size(),
                    playing ? "playing" : "stopped", playing ? "stop" : "play"
                ),
                playing ? ThemePlaying() : ThemeText(), static_cast<int32_t>(c)
            );
        }
    }
}

void DrawSceneExplorer(ZHLN::GUI::Context& ui, InspectorState& state, ZHLN::Engine& engine) {
    const ZHLN::Extent2D winSize = engine.GetWindow().GetSize();

    ui.Panel(
        "glTFSceneExplorer",
        ZHLN::GUI::PanelConfig {
            .width      = kExplorerWidth,
            .height     = static_cast<float>(winSize.height),
            .x          = 0.0f,
            .y          = 0.0f,
            .anchorMinX = 0.0f,
            .anchorMinY = 0.0f,
            .anchorMaxX = 0.0f,
            .anchorMaxY = 0.0f,
            .color      = {0.07f, 0.09f, 0.13f, 0.96f},
            .edgeWidth  = 1.0f,
            .gap        = 2.0f,
            .padding    = kExplorerPadding
        },
        [&]() -> void {
            ui.Label(
                "SCENE EXPLORER",
                ZHLN::GUI::LabelConfig {.scale = 1.0f, .color = {0.30f, 0.85f, 1.0f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 30.0f}
            );

            const ZHLN::ModelPrefab* prefab     = (state.loaded && state.prefab != nullptr) ? state.prefab : nullptr;
            const size_t             modelCount = (prefab != nullptr) ? 1u : 0u;

            ui.Label(
                (prefab != nullptr) ? state.modelName : "no model loaded",
                ZHLN::GUI::LabelConfig {.scale = 0.70f, .color = {0.55f, 0.62f, 0.72f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 18.0f}
            );

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
                        ui.Label(
                            "      (drop a .glb / .gltf onto the window)",
                            ZHLN::GUI::LabelConfig {.scale = 0.65f, .color = {0.45f, 0.50f, 0.58f, 1.0f}, .height = 20.0f}
                        );
                    } else {
                        const std::string modelPath = "scene/models/" + state.modelName;
                        TreeRow(
                            ui, state, modelPath, 2, state.modelName, true, std::format("Model: {}", state.modelName),
                            std::format(
                                "File: {}\nParts: {}  Nodes: {}\nSkeletons: {}  Animations: {}", state.modelName, prefab->parts.size(),
                                prefab->nodes.size(), prefab->skeletons.size(), prefab->animations.size()
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
                ZHLN::GUI::BoxConfig {.height = 178.0f, .color = {0.05f, 0.07f, 0.11f, 0.85f}, .gap = 4.0f, .padding = 10.0f},
                [&]() -> void {
                    ui.Label(
                        state.selectedTitle.empty() ? "Selection" : state.selectedTitle,
                        ZHLN::GUI::LabelConfig {.scale = 0.80f, .color = {0.30f, 0.85f, 1.0f, 1.0f}, .height = 22.0f}
                    );
                    ui.Label(
                        state.selectedDetails.empty() ? "Click an item in the tree to inspect it." : state.selectedDetails,
                        ZHLN::GUI::LabelConfig {
                            .scale = 0.68f, .color = {0.72f, 0.78f, 0.86f, 1.0f}, .verticalAlign = ZHLN::TextVerticalAlignment::Top, .height = 128.0f
                        }
                    );
                }
            );

            ui.Label(
                "LMB drag: orbit | RMB drag: pan | Wheel: zoom",
                ZHLN::GUI::LabelConfig {.scale = 0.60f, .color = {0.45f, 0.50f, 0.58f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 16.0f}
            );
        }
    );
}

// ============================================================================
// LOADING / INPUT
// ============================================================================

void LoadDroppedModel(InspectorState& state, const ZHLN::FileDrop& drop) {
    auto& engine = *state.engine;
    ClearInstances(state);

    ZHLN::ModelPrefab* prefab =
        ZHLN::CreativeWorksFactory::LoadModelPrefabFromMemory(engine, std::span<const uint8_t>(drop.data.data(), drop.data.size()), drop.fileName);
    if (prefab == nullptr) {
        ZHLN::Log("[glTF Inspector] Failed to parse '{}' as glTF.", drop.fileName);
        return;
    }

    const uint32_t capacity = 1u + static_cast<uint32_t>(prefab->parts.size());
    state.instances.resize(static_cast<size_t>(capacity));
    const uint32_t written = ZHLN::CreativeWorksFactory::InstantiatePrefab(
        engine, *prefab, ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .isAnimated = true},
        state.instances.data(), capacity
    );

    ComputeBounds(*prefab, state.target, state.distance);

    // Reset & seed the scene explorer: Scene -> Models -> [model] unfolded.
    state.modelName   = drop.fileName;
    state.expanded    = {"scene", "scene/models", "scene/models/" + state.modelName};
    state.playingClip = prefab->animations.empty() ? -1 : 0;

    state.selectedPath  = "scene/models/" + state.modelName;
    state.selectedTitle = std::format("Model: {}", state.modelName);
    state.selectedDetails =
        std::format(
            "File: {}\nParts: {}  Nodes: {}\nSkeletons: {}  Animations: {}", state.modelName, prefab->parts.size(), prefab->nodes.size(),
            prefab->skeletons.size(), prefab->animations.size()
        );

    state.prefab = prefab;
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
    auto& reg  = engine.GetRegistry();
    auto  ents = reg.GetEntitiesWith<ZHLN::Components::InputStateComponent>();
    if (ents.empty()) {
        return;
    }
    const auto* input = reg.Get<ZHLN::Components::InputStateComponent>(ents[0]);
    if (input == nullptr) {
        return;
    }

    // The scene explorer panel owns input inside its screen region (once shown).
    const bool overExplorer = state.loaded && (input->mouseX >= 0.0f) && (input->mouseX <= kExplorerWidth);
    if (overExplorer) {
        return;
    }

    const float dx  = input->mouseDeltaX;
    const float dy  = input->mouseDeltaY;
    const bool  lmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::LButton));
    const bool  rmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::RButton));
    const bool  mmb = input->IsMouseButtonDownRaw(static_cast<uint8_t>(ZHLN::KeyCode::MButton));

    constexpr float kOrbitSpeed = 0.30f;
    constexpr float kPanSpeed   = 0.0015f;
    constexpr float kZoomSpeed  = 0.15f;

    if (lmb || mmb) {
        // Left Click / Middle Click: Original orbit / turn controls
        state.yaw += dx * kOrbitSpeed;
        state.pitch -= dy * kOrbitSpeed;
        state.pitch = std::clamp(state.pitch, -89.0f, 89.0f);
    } else if (rmb) {
        // Right Click: True canvas grab-and-drag pan
        const JPH::Vec3 forward  = OrbitDirection(state.yaw, state.pitch).Normalized();
        JPH::Vec3       camRight = Cross(forward, JPH::Vec3::sAxisY());
        if (camRight.LengthSq() > 1e-6f) {
            camRight = camRight.Normalized();
        }
        const JPH::Vec3 camUp = Cross(camRight, forward).Normalized();
        const float     scale = state.distance * kPanSpeed;

        // Invert pan offset so dragging pulls the scene with the cursor
        state.target += (-camRight * dx + camUp * dy) * scale;
    }

    if (input->mouseWheel != 0.0f) {
        state.distance *= std::exp(-input->mouseWheel * kZoomSpeed);
        state.distance = std::clamp(state.distance, 0.05f, 5000.0f);
    }

    const JPH::Vec3 dir = OrbitDirection(state.yaw, state.pitch);
    auto&           cam = engine.GetCamera();
    cam.position        = state.target - dir * state.distance;
    cam.yaw             = state.yaw;
    cam.pitch           = state.pitch;
}

void DrawDropPrompt(ZHLN::GUI::Context& ui) {
    ui.Panel(
        "glTFInspectorPrompt",
        ZHLN::GUI::PanelConfig {
            .width = 560.0f, .height = 200.0f, .x = -280.0f, .y = -100.0f, .color = {0.06f, 0.09f, 0.14f, 0.95f}, .gap = 12.0f, .padding = 24.0f
        },
        [&]() -> void {
            ui.Label(
                "glTF Inspector",
                ZHLN::GUI::LabelConfig {.scale = 1.0f, .color = {0.3f, 0.85f, 1.0f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 40.0f}
            );
            ui.Label(
                "Drop a glTF (.glb / .gltf) file onto this window to inspect it.",
                ZHLN::GUI::LabelConfig {.scale = 0.80f, .color = {0.85f, 0.90f, 0.95f, 1.0f}, .align = ZHLN::TextAlignment::Center, .height = 28.0f}
            );
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

    ZHLN::GUI::Context ui(engine.GetRegistry(), engine.GetCurrentFrame());
    // ~Context() sweeps the root cache when 'ui' leaves scope, so no manual
    // end-of-frame sweep is needed here.
    if (state->loaded) {
        DrawSceneExplorer(ui, *state, engine);
    } else {
        DrawDropPrompt(ui);
    }
}

} // namespace

namespace ZHLN::glTF {

void RegisterPrefabLoader() noexcept {
    ZHLN::GLTF::RegisterAsPrefabLoader();
}

void Initialize(ZHLN::Engine& engine) {
    // The inspector's whole purpose is opening model files, and dropping one on
    // the window reaches CreativeWorksFactory, so the importer has to be
    // installed before the first frame can ask for it.
    RegisterPrefabLoader();

    engine.InitializeDefaultScene();
    ZHLN::DefaultPreset::SetDisabled(true);

    {
        auto& reg     = engine.GetRegistry();
        auto  camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
        if (!camEnts.empty()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(camEnts[0]);
        }
    }

    auto* state   = new InspectorState();
    state->engine = &engine;
    engine.SetGameState(state);

    AddInspectorLighting(engine);

    engine.GetWindow().SetFileDropHandler(&OnFileDropped, state);
    engine.SetUICallback([](ZHLN::Engine& eng) -> void { RenderFrame(eng); });

    ZHLN::Log("[glTF Inspector] Initialized. Drop a .glb / .gltf file to begin.");
}

} // namespace ZHLN::glTF
