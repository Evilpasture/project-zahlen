// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GraphicsSettingsSync.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cstdlib>
#include <string_view>

namespace ZHLN {

namespace {

// Namespace-scope aliases (using-declarations cannot name class members
// outside class scope).
using AASettingsComponent          = Components::AASettingsComponent;
using GlobalSettingsTagComponent   = Components::GlobalSettingsTagComponent;
using MainCameraTagComponent       = Components::MainCameraTagComponent;
using PostProcessSettingsComponent = Components::PostProcessSettingsComponent;
using RayTracingSettingsComponent  = Components::RayTracingSettingsComponent;
using ShadowSettingsComponent      = Components::ShadowSettingsComponent;

// Global settings entity (owns the post-process / shadow components).
[[nodiscard]] Entity SettingsEntity(ECS::Registry& reg) noexcept {
    return reg.SingletonEntity<GlobalSettingsTagComponent>();
}

// Main camera entity (owns AASettingsComponent in the default scene).
[[nodiscard]] Entity CameraEntity(ECS::Registry& reg) noexcept {
    return reg.SingletonEntity<MainCameraTagComponent>();
}

// std::array has no operator= from a braced list; build the array by value.
[[nodiscard]] std::array<float, 3> ToArray3(const JPH::Vec3& v) noexcept {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] std::array<float, 4> ToArray4(const JPH::Vec4& v) noexcept {
    return {v.GetX(), v.GetY(), v.GetZ(), v.GetW()};
}

// Slack-driven fidelity governor: the closed-loop pacer reports how early
// each finished frame waits for its V-blank, and this spends that signal.
// While the margin stays under ~2ms the GPU is finishing frames right at the
// deadline -- one hitch away from a miss -- so after 90 consecutive frames
// the governor steps the quality preset down one tier. It never steps up (a
// recovered margin is not proof of headroom, and oscillation is worse than a
// stable tier), never touches Custom (hand-tuned settings are the user's), and
// ZHLN_NO_AUTO_QUALITY opts out entirely. Without a margin -- every policy
// but the closed loop -- this is a counter reset per frame.
constexpr uint64_t kGovernorMarginThresholdNs = 2000000;
constexpr uint32_t kGovernorPatienceFrames    = 90;

// QualityLevel has no formatter (scoped enums log as integers elsewhere),
// so the governor names the tiers itself for its transition line.
[[nodiscard]] constexpr auto QualityName(QualityLevel level) noexcept -> std::string_view {
    switch (level) {
        case QualityLevel::Low: return "Low";
        case QualityLevel::Medium: return "Medium";
        case QualityLevel::High: return "High";
        case QualityLevel::Ultra: return "Ultra";
        case QualityLevel::Custom: return "Custom";
        default: return "Unknown";
    }
}

struct FidelityGovernor {
    uint32_t lowMarginFrames = 0;
    bool     optedOut        = false;
    bool     probedOptOut    = false;
};

void GovernFidelity(Engine& engine, GraphicsSettings& gfx) {
    static FidelityGovernor governor;
    if (!governor.probedOptOut) {
        governor.optedOut     = std::getenv("ZHLN_NO_AUTO_QUALITY") != nullptr;
        governor.probedOptOut = true;
    }

    const PresentTimingMetrics timing = engine.GetRenderContext().GetPresentTiming();
    if (governor.optedOut || !timing.hasMargin || timing.lastPresentMarginNs >= kGovernorMarginThresholdNs) {
        governor.lowMarginFrames = 0;
        return;
    }
    if (++governor.lowMarginFrames < kGovernorPatienceFrames) {
        return;
    }
    governor.lowMarginFrames = 0;

    // gfx.qualityPreset is what CollectGraphicsSettings detected: step down
    // one tier unless the user hand-tuned (Custom) or bottomed out (Low).
    const QualityLevel current = gfx.qualityPreset;
    if (current == QualityLevel::Custom || current == QualityLevel::Low) {
        return;
    }
    const QualityLevel next = static_cast<QualityLevel>(static_cast<uint8_t>(current) - 1);
    if (ApplyQualityPreset(engine, next)) {
        ZHLN::Log(
            "Fidelity governor: present margin under 2ms for {} consecutive frames; stepping quality {} -> {}.", kGovernorPatienceFrames,
            QualityName(current), QualityName(next)
        );
        gfx = CollectGraphicsSettings(engine);
    }
}

} // namespace

GraphicsSettings CollectGraphicsSettings(Engine& engine) {
    auto&            reg = engine.GetRegistry();
    GraphicsSettings gfx {};

    // --- Post-processing / GI / environment (PostProcessSettingsComponent)
    // Preferred: the tagged global settings entity (default-scene layout);
    // fallback: the first entity carrying the component (test scenes) — this
    // preserves the historical Sys_PostProcess / RenderSystem resolution.
    Entity ppEnt = SettingsEntity(reg);
    if (reg.Get<PostProcessSettingsComponent>(ppEnt) == nullptr) {
        ppEnt = reg.SingletonEntity<PostProcessSettingsComponent>();
    }
    if (const auto* pp = reg.Get<PostProcessSettingsComponent>(ppEnt); pp != nullptr) {
        gfx.post.mode          = pp->giMode;
        gfx.post.aoRadius      = pp->aoRadius;
        gfx.post.aoBias        = pp->aoBias;
        gfx.post.aoPower       = pp->aoPower;
        gfx.post.giIntensity   = pp->giIntensity;
        gfx.post.giSamples     = pp->giSamples;
        gfx.post.enableSSR     = pp->enableSSR ? 1 : 0;
        gfx.post.enableRTR     = pp->enableRTR ? 1 : 0;
        gfx.post.glowIntensity = pp->glowIntensity;
        gfx.post.exposure      = pp->exposure;
        gfx.post.bloomStrength = pp->bloomStrength;
        gfx.post.contrast      = pp->contrast;
        gfx.post.saturation    = pp->saturation;
        gfx.post.tonemapper    = pp->tonemapper;
        gfx.post.colorFilter   = ToArray3(pp->colorFilter);

        gfx.environment.ambientExposure = pp->ambientExposure;
        gfx.environment.fullBright      = pp->fullBright;
        gfx.environment.useLocalProbe   = pp->useLocalProbe;
        gfx.environment.probeMin        = ToArray3(pp->probeMin);
        gfx.environment.probeMax        = ToArray3(pp->probeMax);
        gfx.environment.probePos        = ToArray3(pp->probePos);
        gfx.environment.skyZenith       = ToArray4(pp->skyZenith);
        gfx.environment.skyHorizon      = ToArray4(pp->skyHorizon);
        gfx.environment.skyGround       = ToArray4(pp->skyGround);
    }

    // --- Shadows (ShadowSettingsComponent)
    if (const Entity shadowEnt = reg.SingletonEntity<ShadowSettingsComponent>(); shadowEnt != Entity::Null()) {
        if (const auto* shadow = reg.Get<ShadowSettingsComponent>(shadowEnt); shadow != nullptr) {
            gfx.shadows.width              = shadow->shadowWidth;
            gfx.shadows.resolution         = static_cast<uint32_t>(shadow->shadowResolution);
            gfx.shadows.maxPunctualShadows = static_cast<uint32_t>(shadow->maxPunctualShadows);
            gfx.shadows.sunSize            = shadow->sunSize;
        }
    }

    // --- Anti-aliasing (AASettingsComponent; camera-owned in default scene)
    Entity aaEnt = CameraEntity(reg);
    if (reg.Get<AASettingsComponent>(aaEnt) == nullptr) {
        aaEnt = reg.SingletonEntity<AASettingsComponent>();
    }
    if (const auto* aa = reg.Get<AASettingsComponent>(aaEnt); aa != nullptr) {
        gfx.antiAliasing = aa->state;
    }

    // --- Ray tracing
    // Single writer for the semantic toggle ↔ ABI integer pair.
    // Ray tracing knobs live on their own component so presets and debug
    // tools persist; without one, the struct defaults apply every frame.
    if (const Entity rtEnt = reg.SingletonEntity<RayTracingSettingsComponent>(); rtEnt != Entity::Null()) {
        if (const auto* rt = reg.Get<RayTracingSettingsComponent>(rtEnt); rt != nullptr) {
            gfx.rayTracing = rt->config;
        }
    }
    // The reflection toggle stays owned by PostProcessSettings::enableRTR
    // (the in-game menu writes it there); sync it in last so the component
    // cannot shadow the user's menu choice.
    gfx.rayTracing.enableReflections = gfx.post.enableRTR != 0;
    gfx.qualityPreset                = gfx.DetectPreset();
    return gfx;
}

GraphicsSettings SyncGraphicsSettings(Engine& engine) {
    GraphicsSettings gfx = CollectGraphicsSettings(engine);
    // The fidelity governor may step the preset down (re-collecting into gfx
    // when it does), so this runs before the apply, not after.
    GovernFidelity(engine, gfx);
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

    // --- Write-back: post / GI (signature fields only)
    Entity ppEnt = SettingsEntity(reg);
    if (reg.Get<PostProcessSettingsComponent>(ppEnt) == nullptr) {
        ppEnt = reg.SingletonEntity<PostProcessSettingsComponent>();
    }
    if (ppEnt == Entity::Null()) {
        ppEnt = reg.Create(Components::GlobalSettingsTagComponent {});
        reg.Add(ppEnt, PostProcessSettingsComponent {});
        changed = true;
    }
    changed |= reg.Patch<PostProcessSettingsComponent>(ppEnt, [&gfx](PostProcessSettingsComponent& pp) {
        pp.giSamples = gfx.post.giSamples;
        pp.enableSSR = gfx.post.enableSSR;
        pp.enableRTR = gfx.post.enableRTR;
    });

    // --- Write-back: shadows
    Entity shadowEnt = reg.SingletonEntity<ShadowSettingsComponent>();
    if (shadowEnt == Entity::Null()) {
        shadowEnt = SettingsEntity(reg);
        if (shadowEnt != Entity::Null()) {
            reg.Add(shadowEnt, ShadowSettingsComponent {});
        }
    }
    if (shadowEnt != Entity::Null()) {
        changed |= reg.Patch<ShadowSettingsComponent>(shadowEnt, [&gfx](ShadowSettingsComponent& shadow) {
            shadow.shadowResolution = static_cast<int>(gfx.shadows.resolution);
        });
    }

    // --- Write-back: anti-aliasing
    Entity aaEnt = CameraEntity(reg);
    if (reg.Get<AASettingsComponent>(aaEnt) == nullptr) {
        aaEnt = reg.SingletonEntity<AASettingsComponent>();
    }
    if (aaEnt == Entity::Null()) {
        aaEnt = CameraEntity(reg);
        if (aaEnt != Entity::Null()) {
            reg.Add(aaEnt, AASettingsComponent {});
        }
    }
    if (aaEnt != Entity::Null()) {
        changed |= reg.Patch<AASettingsComponent>(aaEnt, [&gfx](AASettingsComponent& aa) {
            aa.state.mode        = gfx.antiAliasing.mode;
            aa.state.taaFeedback = gfx.antiAliasing.taaFeedback;
        });
    }

    // --- Write-back: ray tracing
    // Persist the ray tracing knobs the preset just wrote; without this they
    // would be rebuilt from defaults on the next settings sync.
    Entity rtEnt = reg.SingletonEntity<RayTracingSettingsComponent>();
    if (rtEnt == Entity::Null()) {
        rtEnt = reg.Create();
        reg.Add(rtEnt, RayTracingSettingsComponent {});
        changed = true;
    }
    changed |= reg.Patch<RayTracingSettingsComponent>(rtEnt, [&gfx](RayTracingSettingsComponent& c) { c.config = gfx.rayTracing; });

    if (changed) {
        ZHLN::Log("Graphics quality preset applied: {}", preset);
    }
    return changed;
}

} // namespace ZHLN
