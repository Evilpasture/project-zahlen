// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Minimal COM host for inspecting glTF / GLB assets. Drop a .glb / .gltf file
// onto the window (or pass --model <path>) to upload, animate, and orbit it.

#include <Animation/Context.hpp>
#include <Animation/AnimationSystem.hpp>
#include <Animation/Animator.hpp>
#include <Camera/TargetCamera.hpp>
#include <CommandLine/CommandLine.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <UI/GUI.hpp>
#include <UI/HostUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <glTF/GLTFAssets.hpp>
#include <glTF/GLTFDocument.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct InspectorState {
    ZHLN::COM::IEngine*           engine = nullptr;
    ZHLN::Animation::Context animCtx {};
    std::optional<ZHLN::GLTF::Document> document;
    ZHLN::GLTF::Asset             asset {};
    std::vector<ZHLN::Entity>     spawned;
    ZHLN::Entity                  orbitPivot = ZHLN::Entity::Null();
    std::string                   pendingDrop;
    std::string                   status = "Drop a .glb / .gltf file onto the window";
    uint64_t                      lastGeneration = 0;

    void ClearModel() {
        if (engine == nullptr) return;
        auto& ents = *engine->Entities();
        for (ZHLN::Entity e : spawned) {
            if (ents.IsAlive(e)) ents.Despawn(e);
        }
        spawned.clear();
        if (asset.ownsResources) {
            for (ZHLN::TextureID tex : asset.textures) {
                if (tex != ZHLN::TextureID::Invalid) engine->Graphics()->UnloadTexture(tex);
            }
        }
        asset = {};
        document.reset();
    }

    void LoadPath(std::string_view path) {
        if (engine == nullptr) return;
        auto doc = ZHLN::GLTF::Read(path, {}, engine->Assets());
        if (!doc) {
            status = "Failed to decode: " + std::string(path);
            ZHLN::LogError("[GlTFInspector] {}: {}", status, ZHLN::Error(doc.error()).Message());
            return;
        }
        auto uploaded = ZHLN::GLTF::Upload(*engine->Assets(), *engine->Graphics(), *doc);
        if (!uploaded) {
            status = "Failed to upload: " + std::string(path);
            ZHLN::LogError("[GlTFInspector] {}: {}", status, ZHLN::Error(uploaded.error()).Message());
            return;
        }
        ClearModel();
        const bool animated = !doc->animations.empty();
        const uint32_t capacity = 1U + static_cast<uint32_t>(doc->parts.size()) * 2U + static_cast<uint32_t>(doc->lights.size());
        spawned.resize(capacity);
        ZHLN::PrefabFactory::SpawnParams params {.position = {0.0, 0.0, 0.0}, .createPhysics = false, .isAnimated = animated};
        spawned = engine->Assets()->Instantiate(uploaded->hash, params);
        if (spawned.empty()) {
            status = "Failed to instantiate: " + std::string(path);
            return;
        }
        if (animated && !spawned.empty()) {
            engine->Entities()->Attach(spawned.front(), ZHLN::Animation::AnimatorComponent {.prefabAsset = uploaded->hash, .currentTrackIdx = 0, .currentLoop = true});
        }

        ZHLN::FVec3 bmin {-1.0f, -1.0f, -1.0f};
        ZHLN::FVec3 bmax {1.0f, 1.0f, 1.0f};
        { ZHLN::COM::PrefabBoundsInfo b{}; if (engine->Assets()->GetPrefabBounds(uploaded->hash, ZHLN::COM::OutChain(b)).HasValue() && b.hasParts) { bmin = b.min; bmax = b.max; } }
        const ZHLN::FVec3 center = (bmin + bmax) * 0.5f;
        const float radius = std::max((bmax - bmin).Length() * 0.5f, 0.5f);

        auto& ents = *engine->Entities();
        if (orbitPivot == ZHLN::Entity::Null() || !ents.IsAlive(orbitPivot)) {
            orbitPivot = ents.Spawn(ZHLN::Components::TransformComponent {.position = center});
        } else {
            ents.Patch<ZHLN::Components::TransformComponent>(orbitPivot, [&](auto& t) { t.position = center; });
        }
        const auto camOpt = ents.SingletonEntity<ZHLN::Components::MainCameraTagComponent>();
        const ZHLN::Entity cam = camOpt.HasValue() ? camOpt.value : ZHLN::Entity{};
        if (cam != ZHLN::Entity::Null()) {
            ZHLN::CameraRig::TargetComponent rig {};
            rig.target         = orbitPivot;
            rig.targetOffset   = ZHLN::FVec3::Zero();
            rig.distance       = std::max(radius * 2.4f, 1.5f);
            rig.targetDistance = rig.distance;
            ents.Attach(camOpt.value, rig);
        }

        document       = std::move(*doc);
        asset          = std::move(*uploaded);
        lastGeneration = engine->DeviceGeneration();
        status         = std::string(path);
        ZHLN::Log("[GlTFInspector] Loaded '{}' ({} entity/ies).", path, spawned.size());
    }
};

void OnFileDrop(void* userdata, ZHLN::WindowID, ZHLN::COM::Span<const ZHLN::COM::FileDropView> drops) {
    if (userdata == nullptr || drops.empty()) return;
    auto& state = *static_cast<InspectorState*>(userdata);
    const auto p = std::string_view(drops[0].sourcePath);
    if (!p.empty()) state.pendingDrop = std::string(p);
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    ZHLN::Platform::Init();
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
        .appName = "Zahlen :: glTF Inspector",
    };
    std::string initialModel;
    const std::array extraFlags {
        ZHLN::CliFlag {
            .key         = "--model",
            .placeholder = "<path>",
            .description = "Initial .glb / .gltf asset to open",
            .action      = [&](std::string_view v) -> std::expected<void, ZHLN::CommandLineError> {
                if (v.empty()) return std::unexpected(ZHLN::CommandLineError::MissingValue);
                initialModel.assign(v);
                return {};
            },
        },
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig, extraFlags);
    if (!optionsRes) {
        ZHLN::LogError("[Sample] command line: {}", ZHLN::Error(optionsRes.error()).Message());
        return EXIT_FAILURE;
    }
    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(engineConfig.logLevel);
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::COM::CreateEngineRef(engineConfig);
    if (!engineRes) {
        ZHLN::LogError("Failed to initialize Engine: {}", ZHLN::Error(engineRes.error()).Message());
        return EXIT_FAILURE;
    }
    auto engine = std::move(*engineRes);
    const ZHLN::WindowID primary = engine->PrimaryWindow();
    if (!engineConfig.headless) {
        engine->FocusWindow(primary);
    }

    ZHLN::HostUI hostUI(*engine);
#if defined(ZHLN_HAS_FONTS)
    if (auto fontAsset = ZHLN::Fonts::LoadBakedFont(engine->Assets(), ZHLN::Fonts::VendoredDefaultFontSource()); !fontAsset) {
        ZHLN::LogWarning("Font asset failed to load ({}), using embedded default.", ZHLN::Error(fontAsset.error()).Message());
    } else {
        hostUI.SetBakedFont(std::move(*fontAsset));
    }
#endif

    // Default studio sun + floor so any dropped model reads immediately.
    engine->Assets()->SpawnPlane(24.0f, {0.22f, 0.24f, 0.28f, 1.0f}, {.createPhysics = false});
    engine->Entities()->Spawn(
        ZHLN::Components::SunTagComponent {},
        ZHLN::Components::LightComponent {
            .type        = ZHLN::LightType::Sun,
            .color       = ZHLN::FVec3{1.0f, 0.97f, 0.92f},
            .intensity   = 18.0f,
            .direction   = ZHLN::FVec3{0.40f, 1.0f, 0.35f}.Normalized(),
            .castsShadow = true,
        }
    );

    InspectorState state {.engine = engine.Get(), .lastGeneration = engine->DeviceGeneration()};
    (void)engine->SetFileDropCallback(primary, &OnFileDrop, &state);
    if (!initialModel.empty()) {
        state.LoadPath(initialModel);
    }

    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    while (engine->IsRunning()) {
        const float dt = clock.Tick();
        ZHLN::CheckForCrashes(crashState, engine.Get());
        engine->ProcessEvents();

        if (engine->Actions()->KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine->RequestClose();
            break;
        }
        if (!state.pendingDrop.empty()) {
            const std::string dropped = std::move(state.pendingDrop);
            state.pendingDrop.clear();
            state.LoadPath(dropped);
        }
        if (state.document && engine->DeviceGeneration() != state.lastGeneration) {
            if (ZHLN::GLTF::RebuildGPUResources(*engine, *state.document, state.asset)) {
                state.lastGeneration = engine->DeviceGeneration();
            }
        }

        ZHLN::Animation::Step(*engine, state.animCtx, dt);
        ZHLN::CameraRig::Step(*engine, dt);

        if (!engineConfig.headless) {
            const ZHLN::Extent2D extent = engine->WindowExtent(primary);
            if (extent.width > 0 && extent.height > 0) {
                hostUI.BeginFrame();
                const auto uiInput = hostUI.ConsumeInput(dt, primary);
                auto frame = hostUI.Context().BeginFrame(uiInput);
                frame.Text(state.status, 15.0f, {0.92f, 0.94f, 0.98f, 1.0f});
                (void)hostUI.Submit(std::move(frame).End(), primary);
            }
        }

        const auto result = engine->Tick(dt);
        if (!result.HasValue()) {
            ZHLN::LogError("[GlTFInspector] frame failed: {}", ZHLN::Error(result.error).Message());
            state.ClearModel();
            return EXIT_FAILURE;
        }
    }

    (void)engine->SetFileDropCallback(primary, nullptr, nullptr);
    state.ClearModel();
    return EXIT_SUCCESS;
}
