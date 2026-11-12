// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <VFX/LightningUpdate.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ZHLN::Lightning {
namespace {
auto EvaluateHeidler(float tUs, float i0, float t1, float t2) noexcept -> float {
    if (tUs <= 0.0f || i0 <= 0.0f) {
        return 0.0f;
    }
    const float x  = (tUs / t1) * (tUs / t1);
    const float ec = std::exp(-(t1 / t2) * std::sqrt(2.0f * t2 / t1));
    return (i0 / ec) * (x / (1.0f + x)) * std::exp(-tUs / t2);
}

} // namespace

void Update(LightningUpdateQuery query, ECS::ResMut<GraphicsDevice> graphics, ECS::ResMut<AudioEngine> audioService,
            ECS::ResMut<Entities> entities, FrameDt frameDt) {
    auto rc = *graphics;
    auto audio = *audioService;
    const float dt = frameDt.value;
    const auto ents = query.Entities<LightningComponent>();

    if (ents.empty()) {
        return;
    }

    auto bolts = query.Raw<LightningComponent>();

    std::vector<Entity> deadEntities;
    float               peakLuminanceThisFrame = 0.0f;
    float               unflashedBaseExposure  = 4.5f;
    bool                hasActiveBolts         = false;

    for (size_t i = 0; i < ents.size(); ++i) {
        const Entity        e    = ents[i];
        LightningComponent& bolt = bolts[i];

        if (bolt.phase == LightningPhase::Idle || query.Get<Components::PendingDestroy>(e)) {
            continue;
        }

        // A rebuilt renderer has no cached mesh or material. Restore them from
        // component data, never from an entity-keyed buffer table.
        bool needsMeshRegistration = !rc.Mesh(bolt.meshAssetId).has_value();
        if (bolt.vboPos == BufferID::Invalid || bolt.vboFrame == BufferID::Invalid || bolt.vboSurface == BufferID::Invalid) {
            const auto camComp = query.GetSingleton<Components::CameraComponent>();
            if (!camComp) {
                // No view to face: the ribbon cannot be rebuilt, and the handles stay
                // invalid so the first frame that has a camera retries them. Building
                // it against a fabricated pose would bake the wrong geometry in.
                continue;
            }
            RebuildBuffers(bolt, rc, camComp->camera.position);
            needsMeshRegistration = true;
        }
        if (needsMeshRegistration && bolt.vboPos != BufferID::Invalid && bolt.vboFrame != BufferID::Invalid &&
            bolt.vboSurface != BufferID::Invalid) {
            rc.RegisterMesh(
                bolt.meshAssetId,
                Mesh {.posBuffer = bolt.vboPos, .tangentFrameBuffer = bolt.vboFrame, .surfaceBuffer = bolt.vboSurface, .vertexCount = bolt.visibleVertices}
            );
        }
        if (!rc.Material(bolt.matAssetId)) {
            if (auto mat = rc.CreateMaterial(
                    {.doubleSided = true, .alphaBlend = true, .additiveBlend = true, .alphaMode = 2, .baseColor = {1.0f, 1.0f, 1.0f, 1.0f}}
                )) {
                rc.RegisterMaterial(bolt.matAssetId, *mat);
            }
        }

        unflashedBaseExposure = bolt.baseAmbientExposure;

        const float dtReal = dt / bolt.config.timeDilation;
        bolt.realTime += dtReal;
        bolt.phaseTime += dtReal;

        switch (bolt.phase) {
            case LightningPhase::SteppedLeader: {
                const float growthRate = static_cast<float>(bolt.maxVertices) / 0.04f;
                bolt.visibleVertices += static_cast<uint32_t>(growthRate * dtReal);

                if (bolt.visibleVertices >= bolt.maxVertices) {
                    bolt.visibleVertices = bolt.maxVertices;
                    bolt.phase           = LightningPhase::ReturnStroke;
                    bolt.phaseTime       = 0.0f;

                    audio.PostEvent(
                        {.type     = AudioEventType::OneShot3D,
                         .filepath = "resources/assets/audio/lightning.wav",
                         .position = bolt.groundTarget,
                         .volume   = bolt.config.soundVolume}
                    );
                }
                break;
            }

            case LightningPhase::ReturnStroke: {
                const float tNorm = std::clamp(bolt.phaseTime / 0.15f, 0.0f, 1.0f);
                const float tUs   = tNorm * 200.0f;
                const float iNow  = EvaluateHeidler(tUs, bolt.config.peakCurrentKA, 1.8f, 95.0f);

                bolt.currentKA      = iNow;
                bolt.flashLuminance = std::pow(std::max(iNow, 0.0f) / 30.0f, 1.4f);

                if (bolt.phaseTime > 0.15f) {
                    bolt.phase     = LightningPhase::Dissipating;
                    bolt.phaseTime = 0.0f;
                }
                break;
            }

            case LightningPhase::Dissipating: {
                const float fade    = std::exp(-bolt.phaseTime * 8.0f);
                bolt.flashLuminance = fade * 0.2f;

                if (fade < 0.01f) {
                    bolt.phase = LightningPhase::Idle;
                    deadEntities.push_back(e);
                    continue;
                }
                break;
            }
            default:
                break;
        }

        hasActiveBolts         = true;
        peakLuminanceThisFrame = std::max(peakLuminanceThisFrame, bolt.flashLuminance);

        if (auto gpuMatOpt = rc.Material(bolt.matAssetId)) {
            Material    mat       = *gpuMatOpt;
            const float intensity = (bolt.phase == LightningPhase::SteppedLeader) ? 15.0f : (bolt.flashLuminance * bolt.config.emissiveIntensity);

            // Lanes 0-2 only: the fourth lane belongs to the ABI's emissive
            // packing and this path never owned it.
            mat.emissiveFactor.x  = 0.88f * intensity;
            mat.emissiveFactor.y  = 0.95f * intensity;
            mat.emissiveFactor.z  = 1.00f * intensity;
            rc.RegisterMaterial(bolt.matAssetId, mat);
        }

        if (auto gpuMeshOpt = rc.Mesh(bolt.meshAssetId)) {
            Mesh m        = *gpuMeshOpt;
            m.vertexCount = bolt.visibleVertices;
            rc.RegisterMesh(bolt.meshAssetId, m);
        }

        if (query.IsAlive(bolt.flashLightEntity)) {
            query.Patch<Components::LightComponent>(bolt.flashLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 8000000.0f; });
        }
        if (query.IsAlive(bolt.impactLightEntity)) {
            query.Patch<Components::LightComponent>(bolt.impactLightEntity, [&](auto& light) { light.intensity = bolt.flashLuminance * 4000000.0f; });
        }
    }

    const auto setAmbientExposure = [&rc](float value) {
        GraphicsSettings gfx            = rc.Settings();
        gfx.environment.ambientExposure = value;
        rc.ApplySettings(gfx);
    };
    if (hasActiveBolts) {
        setAmbientExposure(unflashedBaseExposure + (180.0f * peakLuminanceThisFrame));
    }

    for (const Entity deadEnt: deadEntities) {
        // The scene cleanup pass releases VBOs and child lights after the
        // simulation step, while the LightningComponent is still inspectable.
        entities->Despawn(deadEnt);
    }

    if (!hasActiveBolts) {
        setAmbientExposure(unflashedBaseExposure);
    }
}
} // namespace ZHLN::Lightning
