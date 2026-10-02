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

using AASettingsComponent          = Components::AASettingsComponent;
using GlobalSettingsTagComponent   = Components::GlobalSettingsTagComponent;
using MainCameraTagComponent       = Components::MainCameraTagComponent;
using PostProcessSettingsComponent = Components::PostProcessSettingsComponent;
using RayTracingSettingsComponent  = Components::RayTracingSettingsComponent;
using ShadowSettingsComponent      = Components::ShadowSettingsComponent;

[[nodiscard]] Entity SettingsEntity(ECS::Registry& reg) noexcept {
    return reg.SingletonEntity<GlobalSettingsTagComponent>();
}

[[nodiscard]] Entity CameraEntity(ECS::Registry& reg) noexcept {
    return reg.SingletonEntity<MainCameraTagComponent>();
}

[[nodiscard]] std::array<float, 3> ToArray3(const JPH::Vec3& v) noexcept {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] std::array<float, 4> ToArray4(const JPH::Vec4& v) noexcept {
    return {v.GetX(), v.GetY(), v.GetZ(), v.GetW()};
}

constexpr uint64_t kGovernorMarginThresholdNs = 2000000;
constexpr uint32_t kGovernorPatienceFrames    = 90;

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

}

GraphicsSettings CollectGraphicsSettings(Engine& engine) {
    auto&            reg = engine.GetRegistry();
    GraphicsSettings gfx {};

    Entity ppEnt = SettingsEntity(reg);
    if (!reg.Get<PostProcessSettingsComponent>(ppEnt)) {
        ppEnt = reg.SingletonEntity<PostProcessSettingsComponent>();
    }
    if (const auto pp = reg.Get<PostProcessSettingsComponent>(ppEnt)) {
        gfx.post.mode              = pp->giMode;
        gfx.post.aoRadius          = pp->aoRadius;
        gfx.post.aoBias            = pp->aoBias;
        gfx.post.aoPower           = pp->aoPower;
        gfx.post.giIntensity       = pp->giIntensity;
        gfx.post.giSamples         = pp->giSamples;
        gfx.post.vignetteIntensity = pp->vignetteIntensity;
        gfx.post.vignettePower     = pp->vignettePower;
        gfx.post.enableSSR         = pp->enableSSR ? 1 : 0;
        gfx.post.enableRTR         = pp->enableRTR ? 1 : 0;
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

    if (const Entity shadowEnt = reg.SingletonEntity<ShadowSettingsComponent>(); shadowEnt != Entity::Null()) {
        if (const auto shadow = reg.Get<ShadowSettingsComponent>(shadowEnt)) {
            gfx.shadows.width              = shadow->shadowWidth;
            gfx.shadows.resolution         = static_cast<uint32_t>(shadow->shadowResolution);
            gfx.shadows.maxPunctualShadows = static_cast<uint32_t>(shadow->maxPunctualShadows);
            gfx.shadows.sunSize            = shadow->sunSize;
        }
    }

    Entity aaEnt = CameraEntity(reg);
    if (!reg.Get<AASettingsComponent>(aaEnt)) {
        aaEnt = reg.SingletonEntity<AASettingsComponent>();
    }
    if (const auto aa = reg.Get<AASettingsComponent>(aaEnt)) {
        gfx.antiAliasing = aa->state;
    }

    if (const Entity rtEnt = reg.SingletonEntity<RayTracingSettingsComponent>(); rtEnt != Entity::Null()) {
        if (const auto rt = reg.Get<RayTracingSettingsComponent>(rtEnt)) {
            gfx.rayTracing = rt->config;
        }
    }
    gfx.rayTracing.enableReflections = gfx.post.enableRTR != 0;
    gfx.qualityPreset                = gfx.DetectPreset();
    return gfx;
}

GraphicsSettings SyncGraphicsSettings(Engine& engine) {
    GraphicsSettings gfx = CollectGraphicsSettings(engine);
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

    Entity ppEnt = SettingsEntity(reg);
    if (!reg.Get<PostProcessSettingsComponent>(ppEnt)) {
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

    Entity aaEnt = CameraEntity(reg);
    if (!reg.Get<AASettingsComponent>(aaEnt)) {
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

}
