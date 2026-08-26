// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GraphicsSettingsSync.hpp"

#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>

namespace ZHLN {

namespace {

using Components::AASettingsComponent;
using Components::GlobalSettingsTagComponent;
using Components::MainCameraTagComponent;
using Components::PostProcessSettingsComponent;
using Components::ShadowSettingsComponent;

[[nodiscard]] Entity FirstOrNull(std::span<const Entity> entities) noexcept {
    return entities.empty() ? Entity::Null() : entities[0];
}

/// Global settings entity (owns the post-process / shadow components).
[[nodiscard]] Entity SettingsEntity(ECS::Registry& reg) noexcept {
    return FirstOrNull(reg.GetEntitiesWith<GlobalSettingsTagComponent>());
}

/// Main camera entity (owns AASettingsComponent in the default scene).
[[nodiscard]] Entity CameraEntity(ECS::Registry& reg) noexcept {
    return FirstOrNull(reg.GetEntitiesWith<MainCameraTagComponent>());
}

} // namespace

GraphicsSettings CollectGraphicsSettings(Engine& engine) {
    auto&            reg = engine.GetRegistry();
    GraphicsSettings gfx {};

    // --- Post-processing / GI / environment (PostProcessSettingsComponent) ---
    // Preferred: the tagged global settings entity (default-scene layout);
    // fallback: the first entity carrying the component (test scenes) — this
    // preserves the historical Sys_PostProcess / RenderSystem resolution.
    Entity ppEnt = SettingsEntity(reg);
    if (reg.Get<PostProcessSettingsComponent>(ppEnt) == nullptr) {
        ppEnt = FirstOrNull(reg.GetEntitiesWith<PostProcessSettingsComponent>());
    }
    if (const auto* pp = reg.Get<PostProcessSettingsComponent>(ppEnt); pp != nullptr) {
        gfx.post.mode        = pp->giMode;
        gfx.post.aoRadius    = pp->aoRadius;
        gfx.post.aoBias      = pp->aoBias;
        gfx.post.aoPower     = pp->aoPower;
        gfx.post.giIntensity = pp->giIntensity;
        gfx.post.giSamples   = pp->giSamples;
        gfx.post.enableSSR   = pp->enableSSR ? 1 : 0;
        gfx.post.enableRTR   = pp->enableRTR ? 1 : 0;

        gfx.environment.ambientExposure = pp->ambientExposure;
        gfx.environment.fullBright      = pp->fullBright;
        gfx.environment.useLocalProbe   = pp->useLocalProbe;
        gfx.environment.probeMin        = {pp->probeMin.GetX(), pp->probeMin.GetY(), pp->probeMin.GetZ()};
        gfx.environment.probeMax        = {pp->probeMax.GetX(), pp->probeMax.GetY(), pp->probeMax.GetZ()};
        gfx.environment.probePos        = {pp->probePos.GetX(), pp->probePos.GetY(), pp->probePos.GetZ()};
        gfx.environment.skyZenith       = {pp->skyZenith.GetX(), pp->skyZenith.GetY(), pp->skyZenith.GetZ(), pp->skyZenith.GetW()};
        gfx.environment.skyHorizon      = {pp->skyHorizon.GetX(), pp->skyHorizon.GetY(), pp->skyHorizon.GetZ(), pp->skyHorizon.GetW()};
        gfx.environment.skyGround       = {pp->skyGround.GetX(), pp->skyGround.GetY(), pp->skyGround.GetZ(), pp->skyGround.GetW()};
    }

    // --- Shadows (ShadowSettingsComponent) ---
    if (const Entity shadowEnt = FirstOrNull(reg.GetEntitiesWith<ShadowSettingsComponent>()); shadowEnt != Entity::Null()) {
        if (const auto* shadow = reg.Get<ShadowSettingsComponent>(shadowEnt); shadow != nullptr) {
            gfx.shadows.width              = shadow->shadowWidth;
            gfx.shadows.resolution         = static_cast<uint32_t>(shadow->shadowResolution);
            gfx.shadows.maxPunctualShadows = static_cast<uint32_t>(shadow->maxPunctualShadows);
            gfx.shadows.sunSize            = shadow->sunSize;
        }
    }

    // --- Anti-aliasing (AASettingsComponent; camera-owned in default scene) ---
    Entity aaEnt = CameraEntity(reg);
    if (reg.Get<AASettingsComponent>(aaEnt) == nullptr) {
        aaEnt = FirstOrNull(reg.GetEntitiesWith<AASettingsComponent>());
    }
    if (const auto* aa = reg.Get<AASettingsComponent>(aaEnt); aa != nullptr) {
        gfx.antiAliasing = aa->state;
    }

    // --- Ray tracing ---------------------------------------------------------
    // Single writer for the semantic toggle ↔ ABI integer pair.
    gfx.rayTracing.enableReflections = gfx.post.enableRTR != 0;
    gfx.qualityPreset                = gfx.DetectPreset();
    return gfx;
}

GraphicsSettings SyncGraphicsSettings(Engine& engine) {
    GraphicsSettings gfx = CollectGraphicsSettings(engine);
    engine.GetRenderContext().ApplySettings(gfx);
    return gfx;
}

bool ApplyQualityPreset(Engine& engine, QualityLevel preset) {
    if (preset == QualityLevel::Custom) {
        return false;
    }

    auto&            reg = engine.GetRegistry();
    GraphicsSettings gfx = CollectGraphicsSettings(engine);
    gfx.ApplyPreset(preset);
    bool changed = false;

    // --- Write-back: post / GI (signature fields only) -----------------------
    Entity ppEnt = SettingsEntity(reg);
    if (reg.Get<PostProcessSettingsComponent>(ppEnt) == nullptr) {
        ppEnt = FirstOrNull(reg.GetEntitiesWith<PostProcessSettingsComponent>());
    }
    if (ppEnt == Entity::Null()) {
        ppEnt = reg.Create(Components::GlobalSettingsTagComponent {});
        reg.Add(ppEnt, PostProcessSettingsComponent{});
        changed = true;
    }
    changed |= reg.Patch<PostProcessSettingsComponent>(
        ppEnt,
        [&gfx](PostProcessSettingsComponent& pp) {
            pp.giSamples   = gfx.post.giSamples;
            pp.enableSSR   = gfx.post.enableSSR;
            pp.enableRTR   = gfx.post.enableRTR;
        }
    );

    // --- Write-back: shadows --------------------------------------------------
    Entity shadowEnt = FirstOrNull(reg.GetEntitiesWith<ShadowSettingsComponent>());
    if (shadowEnt == Entity::Null()) {
        shadowEnt = SettingsEntity(reg);
        if (shadowEnt != Entity::Null()) {
            reg.Add(shadowEnt, ShadowSettingsComponent{});
        }
    }
    if (shadowEnt != Entity::Null()) {
        changed |= reg.Patch<ShadowSettingsComponent>(
            shadowEnt,
            [&gfx](ShadowSettingsComponent& shadow) {
                shadow.shadowResolution = static_cast<int>(gfx.shadows.resolution);
            }
        );
    }

    // --- Write-back: anti-aliasing ---------------------------------------------
    Entity aaEnt = CameraEntity(reg);
    if (reg.Get<AASettingsComponent>(aaEnt) == nullptr) {
        aaEnt = FirstOrNull(reg.GetEntitiesWith<AASettingsComponent>());
    }
    if (aaEnt == Entity::Null()) {
        aaEnt = CameraEntity(reg);
        if (aaEnt != Entity::Null()) {
            reg.Add(aaEnt, AASettingsComponent{});
        }
    }
    if (aaEnt != Entity::Null()) {
        changed |= reg.Patch<AASettingsComponent>(
            aaEnt,
            [&gfx](AASettingsComponent& aa) {
                aa.state.mode        = gfx.antiAliasing.mode;
                aa.state.taaFeedback = gfx.antiAliasing.taaFeedback;
            }
        );
    }

    if (changed) {
        ZHLN::Log("Graphics quality preset applied: {}", ToString(preset));
    }
    return changed;
}

} // namespace ZHLN
