// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <FallbackScene/FallbackScene.hpp>
#include <Terrain/TerrainHost.hpp>
#include <VFX/CombatFX.hpp>
#include <Zahlen/HostUI.hpp>
#include <cmath>
namespace ZHLN::Tools {
// Real scene-host composition: noise/tint terrain, a heightfield collider,
// falling rigid bodies, decals, tracers, sparks, rings and procedural audio.
// No Engine facade, render graph registration, registry or engine service.
struct TerrainDiagnostic {
    Terrain::Host    terrain;
    CombatFX::System combat;
    bool             active     = false;
    bool             paused     = false;
    float            time       = 0;
    float            nextImpact = 0;
    float            dt         = 0;
    auto             Start(COM::IEngine& engine, bool buildBLAS) -> std::expected<void, ErrorCode> {
        auto description                  = FallbackScene::DefaultPreset::FallbackScene();
        description.name                  = "Host terrain and combat";
        description.camera                = {.position = {0, 18, 32}, .pitch = -22};
        description.environment.enableRTR = buildBLAS;
        description.entities.clear();
        for (int i = 0; i < 8; ++i) {
            Scene::SceneEntity stone;
            stone.name               = "TerrainDrop";
            stone.body               = Scene::BodyKind::Dynamic;
            stone.halfExtents        = {0.35f, 0.35f, 0.35f};
            stone.transform.position = {float(i * 2 - 7), float(10 + i), 0};
            stone.material.baseColor = {.35f, .3f, .25f, 1};
            description.entities.push_back(stone);
        }
        if (auto scene = Scene::Instantiate(engine, description, {.buildBLAS = buildBLAS}); !scene)
            return std::unexpected(scene.error());
        terrain.Start(engine);
        auto geometry = Terrain::Generate({.sampleCount = 64, .worldSize = 70, .maxHeight = 12, .type = Terrain::TerrainType::Snow});
        if (!geometry)
            return std::unexpected(geometry.error());
        if (auto tile = terrain.Create(std::move(*geometry), {.position = {0, -5, 0}, .buildBLAS = buildBLAS}); !tile)
            return std::unexpected(tile.error());
        if (auto started = combat.Start(engine); !started)
            return started;
        active     = true;
        paused     = false;
        time       = 0;
        nextImpact = 0;
        return {};
    }
    auto Tick(COM::IEngine& engine, float delta) -> std::expected<void, ErrorCode> {
        dt = delta;
        if (!active || paused)
            return {};
        time += delta;
        nextImpact -= delta;
        if (nextImpact <= 0) {
            nextImpact    = .7f;
            const float x = std::sin(time * .65f) * 6, z = std::cos(time * .7f) * 4;
            const FVec3 point {x, terrain.SampleHeightAt(x, z) + .04f, z};
            if (auto impact = combat.SpawnImpact(engine, point, FVec3::UnitY(), 2); !impact)
                return impact;
            combat.SpawnTracer({0, 12, 20}, point, 90, 2);
            combat.SpawnShockwaveRing(point, FVec3::UnitY(), 2, .5f);
        }
        return {};
    }
    auto BeforeRender(COM::IEngine& engine, float delta) -> std::expected<void, ErrorCode> {
        if (!active)
            return {};
        if (auto ready = terrain.BeforeRender(engine, delta); !ready)
            return ready;
        return combat.BeforeRender(engine, paused ? 0 : delta);
    }
    auto UI(COM::IEngine& engine, HostUI& hostUI) -> std::expected<void, ErrorCode> {
        if (!active)
            return {};
        auto ui = hostUI.Context().BeginFrame(hostUI.ConsumeInput(dt));
        ui.BeginBox(
            "HostTerrainPanel",
            GUI::BoxConfig {.width = {.fixed = 300}, .color = {.05f, .07f, .1f, .9f}, .padding = 12, .gap = 8, .direction = GUI::Direction::Column}
        );
        ui.Text("COM TERRAIN + COMBAT", 18, {.35f, .88f, 1, 1});
        ui.Text("Snow terrain / heightfield / falling bodies\nHost-owned decals, sparks, tracers and rings", 14, {.9f, .9f, .9f, 1});
        if (ui.Button(paused ? "Resume effects" : "Pause effects"))
            paused = !paused;
        if (ui.Button("Quit"))
            engine.RequestClose();
        ui.EndBox();
        auto result = hostUI.Submit(std::move(ui).End());
        if (!result.HasValue())
            return std::unexpected(result.error);
        return {};
    }
    void Stop(COM::IEngine& engine) {
        combat.Stop(engine);
        terrain.Stop(engine);
        active = false;
    }
};
} // namespace ZHLN::Tools
