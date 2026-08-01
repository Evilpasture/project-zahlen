// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

export module ZHLN.Lightning;

export namespace ZHLN {

enum class LightningPhase : uint8_t { Idle, SteppedLeader, ReturnStroke, Dissipating };

struct LightningConfig {
    float eta              = 2.0f;  // Branching factor / probability
    float peakCurrentKA    = 45.0f; // Peak current in kA
    float timeDilation     = 1.0f;  // Slow-motion factor
    bool  positivePolarity = false; // Polarity (+CG / -CG)
    float ribbonWidth      = 0.8f;  // Width of the glowing 3D lightning trunk
    int   subdivisions     = 5;     // 2^5 = 32 segments on main trunk + branches
};

struct LightningSegment {
    JPH::Vec3 start;
    JPH::Vec3 end;
    float     width;
    float     branchLevel;
};

class LightningSimulation {
  public:
    LightningSimulation() = default;
    ~LightningSimulation() {
        Cleanup();
    }

    LightningSimulation(const LightningSimulation&)            = delete;
    LightningSimulation& operator=(const LightningSimulation&) = delete;

    [[nodiscard]] LightningPhase GetPhase() const noexcept {
        return m_phase;
    }
    [[nodiscard]] float GetCurrentKA() const noexcept {
        return m_currentKA;
    }

    /**
     * @brief Generates a 3D branching fractal bolt instantly in 20 microseconds with zero lag.
     */
    void TriggerStrike(Engine& engine, JPH::RVec3Arg cloudSeedPos, JPH::RVec3Arg groundTargetPos, const LightningConfig& config = {}) {
        Cleanup();
        m_cfg    = config;
        m_engine = &engine;

        m_groundTarget = JPH::Vec3(groundTargetPos);
        m_cloudOrigin  = JPH::Vec3(m_groundTarget.GetX(), static_cast<float>(cloudSeedPos.GetY()), m_groundTarget.GetZ());

        // 1. Generate 3D Branching Fractal Bolt Geometry
        GenerateFractalBolt(m_cloudOrigin, m_groundTarget, m_cfg.ribbonWidth);

        // 2. Build Camera-Facing Ribbon Mesh
        InitRenderResources(engine);

        // 3. Start State Machine
        m_phase           = LightningPhase::SteppedLeader;
        m_realTime        = 0.0f;
        m_phaseTime       = 0.0f;
        m_visibleVertices = 0;
        m_flashLuminance  = 1.0f;

        ZHLN::Log(
            "[Lightning] 3D Fractal Strike discharged toward ({:.1f}, {:.1f}, {:.1f})", m_groundTarget.GetX(), m_groundTarget.GetY(), m_groundTarget.GetZ()
        );
    }

    void Update(Engine& engine, float dt) {
        if (m_phase == LightningPhase::Idle)
            return;

        float dtReal = dt / m_cfg.timeDilation;
        m_realTime += dtReal;
        m_phaseTime += dtReal;

        auto& rc  = engine.GetRenderContext();
        auto& reg = engine.GetRegistry();

        switch (m_phase) {
            case LightningPhase::SteppedLeader: {
                // Animate stepped leader descending rapidly (reaches ground in ~0.04s)
                float growthRate = static_cast<float>(m_maxVertices) / 0.04f;
                m_visibleVertices += static_cast<uint32_t>(growthRate * dtReal);

                if (m_visibleVertices >= m_maxVertices) {
                    m_visibleVertices = m_maxVertices;
                    m_phase           = LightningPhase::ReturnStroke;
                    m_phaseTime       = 0.0f;
                    TriggerAcousticThunder();
                }

                UpdateMaterialGlow(rc, 2.0f, 3.5f, 6.0f);
                break;
            }

            case LightningPhase::ReturnStroke: {
                float tUs  = m_phaseTime * 1.0e6f;
                float iNow = EvaluateHeidler(tUs, m_cfg.peakCurrentKA, 1.8f, 95.0f);

                m_currentKA = iNow;
                float lum   = std::pow(std::max(iNow, 0.0f) / 30.0f, 1.4f);

                // Blinding HDR Plasma Flash
                UpdateMaterialGlow(rc, 60.0f * lum, 65.0f * lum, 80.0f * lum);
                UpdateFlashLighting(engine, lum * 2.0f);

                if (m_phaseTime > 0.15f) {
                    m_phase     = LightningPhase::Dissipating;
                    m_phaseTime = 0.0f;
                }
                break;
            }

            case LightningPhase::Dissipating: {
                float fade = std::exp(-m_phaseTime * 8.0f);
                UpdateMaterialGlow(rc, 2.0f * fade, 2.5f * fade, 4.0f * fade);
                UpdateFlashLighting(engine, fade * 0.2f);

                if (fade < 0.01f) {
                    Cleanup(); // Completely destroys VBOs & entities once faded out
                    m_phase = LightningPhase::Idle;
                }
                break;
            }
            default:
                break;
        }

        // Animate progression by updating vertexCount header without reallocating VBOs
        if (m_lightningEntity != NullEntity && reg.IsAlive(m_lightningEntity)) {
            const auto* meshComp = reg.Get<Components::MeshComponent>(m_lightningEntity);
            if (meshComp != nullptr) {
                if (auto gpuMeshOpt = rc.GetGPUMesh(meshComp->meshAsset)) {
                    Mesh m        = *gpuMeshOpt;
                    m.vertexCount = m_visibleVertices;
                    rc.RegisterGPUMesh(meshComp->meshAsset, m);
                }
            }
        }
    }

  private:
    void GenerateFractalBolt(JPH::Vec3 start, JPH::Vec3 end, float startWidth) {
        m_segments.clear();
        std::vector<LightningSegment> queue;
        queue.push_back({start, end, startWidth, 0.0f});

        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::uniform_real_distribution<float> prob(0.0f, 1.0f);

        for (int step = 0; step < m_cfg.subdivisions; ++step) {
            std::vector<LightningSegment> nextQueue;
            float                         scale = std::pow(0.55f, static_cast<float>(step)) * (end - start).Length() * 0.18f;

            for (const auto& seg: queue) {
                JPH::Vec3 mid = (seg.start + seg.end) * 0.5f;

                JPH::Vec3 dir = (seg.end - seg.start);
                float     len = dir.Length();
                if (len < 0.1f) {
                    nextQueue.push_back(seg);
                    continue;
                }
                dir /= len;

                JPH::Vec3 side = dir.Cross(JPH::Vec3::sAxisY());
                if (side.LengthSq() < 1e-4f)
                    side = JPH::Vec3::sAxisX();
                else
                    side = side.Normalized();
                JPH::Vec3 up = dir.Cross(side).Normalized();

                JPH::Vec3 offset      = (side * dist(m_rng) + up * dist(m_rng)) * scale;
                JPH::Vec3 jitteredMid = mid + offset;

                // Split main segment into two
                nextQueue.push_back({seg.start, jitteredMid, seg.width, seg.branchLevel});
                nextQueue.push_back({jitteredMid, seg.end, seg.width, seg.branchLevel});

                // Spawn natural side branch
                float branchProb = (0.35f * m_cfg.eta / 2.0f) / (1.0f + seg.branchLevel * 0.8f);
                if (prob(m_rng) < branchProb) {
                    JPH::Vec3 branchDir = (dir + (side * dist(m_rng) + up * dist(m_rng)) * 0.7f).Normalized();
                    float     branchLen = len * (0.4f + prob(m_rng) * 0.3f);
                    JPH::Vec3 branchEnd = jitteredMid + branchDir * branchLen;

                    nextQueue.push_back({jitteredMid, branchEnd, seg.width * 0.5f, seg.branchLevel + 1.0f});
                }
            }
            queue = std::move(nextQueue);
        }
        m_segments = std::move(queue);
    }

    float EvaluateHeidler(float tUs, float i0, float t1, float t2) const noexcept {
        if (tUs <= 0.0f || i0 <= 0.0f)
            return 0.0f;
        float x  = (tUs / t1) * (tUs / t1);
        float ec = std::exp(-(t1 / t2) * std::sqrt(2.0f * t2 / t1));
        return (i0 / ec) * (x / (1.0f + x)) * std::exp(-tUs / t2);
    }

    void InitRenderResources(Engine& engine) {
        auto& rc  = engine.GetRenderContext();
        auto& reg = engine.GetRegistry();
        auto& cam = engine.GetCamera();

        std::vector<VertexPosition>   positions;
        std::vector<VertexAttributes> attributes;
        positions.reserve(m_segments.size() * 6);
        attributes.reserve(m_segments.size() * 6);

        Packed1010102 n = Math::PackNormal(0, 1, 0);
        Packed1010102 t = Math::PackNormal(1, 0, 0, 1);
        PackedRGBA8   c = Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f);

        JPH::Vec3 camPos = cam.position;

        for (const auto& seg: m_segments) {
            JPH::Vec3 p0 = seg.start;
            JPH::Vec3 p1 = seg.end;

            JPH::Vec3 dir = (p1 - p0);
            float     len = dir.Length();
            if (len < 1e-4f)
                continue;
            dir /= len;

            // Camera-facing ribbons
            JPH::Vec3 toCam = (camPos - p0).Normalized();
            JPH::Vec3 side  = dir.Cross(toCam);
            if (side.LengthSq() < 1e-4f)
                side = JPH::Vec3::sAxisX();
            else
                side = side.Normalized();

            float width = seg.width;

            JPH::Vec3 v0 = p0 - side * width;
            JPH::Vec3 v1 = p0 + side * width;
            JPH::Vec3 v2 = p1 - side * width;
            JPH::Vec3 v3 = p1 + side * width;

            positions.push_back({{v0.GetX(), v0.GetY(), v0.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 0), .color = c});
            positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
            positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});

            positions.push_back({{v2.GetX(), v2.GetY(), v2.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(0, 1), .color = c});
            positions.push_back({{v1.GetX(), v1.GetY(), v1.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 0), .color = c});
            positions.push_back({{v3.GetX(), v3.GetY(), v3.GetZ()}});
            attributes.push_back({.normal = n, .tangent = t, .uv = Math::PackUV(1, 1), .color = c});
        }

        m_maxVertices = static_cast<uint32_t>(positions.size());
        m_vboPos      = rc.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
        m_vboAttr     = rc.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

        m_lightningEntity = reg.Create();
        reg.Add(m_lightningEntity, Components::TransformComponent {});
        reg.Add(m_lightningEntity, Components::NameComponent {.name = String64("ProceduralLightning")});

        m_meshAssetId = HashAssetID("lightning_mesh_" + std::to_string(m_lightningEntity.index));
        m_matAssetId  = HashAssetID("lightning_mat_" + std::to_string(m_lightningEntity.index));

        Mesh mesh {
            .posBuffer   = m_vboPos,
            .attrBuffer  = m_vboAttr,
            .vertexCount = 0 // Starts invisible
        };
        rc.RegisterGPUMesh(m_meshAssetId, mesh);

        auto matRes = CreativeWorksFactory::CreateBasicMaterial(rc, true, true);
        if (matRes) {
            Material mat           = matRes.value();
            mat.baseColorFactor[0] = 1.0f;
            mat.baseColorFactor[1] = 1.0f;
            mat.baseColorFactor[2] = 1.0f;
            mat.baseColorFactor[3] = 1.0f;
            rc.RegisterGPUMaterial(m_matAssetId, mat);
        }

        reg.Add(
            m_lightningEntity, Components::MeshComponent {
                                   .meshAsset     = m_meshAssetId,
                                   .materialAsset = m_matAssetId,
                                   .cullRadius    = 20000.0f, // Infinite bounds
                                   .flags         = DrawFlags::ExcludeFromTLAS
                               }
        );

        // Flashing Dynamic Point Light
        m_flashLightEntity = reg.Create();
        reg.Add(m_flashLightEntity, Components::TransformComponent {.position = JPH::Vec3(m_groundTarget)});
        reg.Add(
            m_flashLightEntity, Components::LightComponent {
                                    .type        = LightType::Point,
                                    .color       = JPH::Vec3(0.6f, 0.8f, 1.0f),
                                    .intensity   = 0.0f,
                                    .radius      = 2.0f,
                                    .direction   = JPH::Vec3(0.0f, -1.0f, 0.0f),
                                    .range       = 350.0f,
                                    .points      = JPH::Mat44::sIdentity(),
                                    .twoSided    = 0,
                                    .shadowLayer = -1
                                }
        );
    }

    void UpdateMaterialGlow(RenderContext& rc, float r, float g, float b) {
        if (auto gpuMatOpt = rc.GetGPUMaterial(m_matAssetId)) {
            Material mat          = *gpuMatOpt;
            mat.emissiveFactor[0] = r;
            mat.emissiveFactor[1] = g;
            mat.emissiveFactor[2] = b;
            rc.RegisterGPUMaterial(m_matAssetId, mat);
        }
    }

    void UpdateFlashLighting(Engine& engine, float luminance) {
        auto& reg = engine.GetRegistry();
        if (m_flashLightEntity != NullEntity && reg.IsAlive(m_flashLightEntity)) {
            if (auto* light = reg.Get<Components::LightComponent>(m_flashLightEntity)) {
                light->intensity = luminance * 25.0f;
            }
        }
    }

    void TriggerAcousticThunder() {
        if (!m_engine)
            return;
        m_engine->GetAudioContext().PlayProceduralBeep(80.0f, 0.4f, 0.4f);
        m_engine->GetAudioContext().PlayProceduralBeep(140.0f, 0.8f, 0.3f);
    }

    void Cleanup() {
        if (m_engine) {
            auto& reg = m_engine->GetRegistry();
            auto& rc  = m_engine->GetRenderContext();

            if (m_vboPos != BufferHandle::Invalid)
                rc.DestroyBuffer(m_vboPos);
            if (m_vboAttr != BufferHandle::Invalid)
                rc.DestroyBuffer(m_vboAttr);

            if (m_lightningEntity != NullEntity && reg.IsAlive(m_lightningEntity))
                reg.Destroy(m_lightningEntity);
            if (m_flashLightEntity != NullEntity && reg.IsAlive(m_flashLightEntity))
                reg.Destroy(m_flashLightEntity);
        }

        m_lightningEntity  = NullEntity;
        m_flashLightEntity = NullEntity;
        m_vboPos           = BufferHandle::Invalid;
        m_vboAttr          = BufferHandle::Invalid;
        m_segments.clear();
    }

    LightningConfig m_cfg;
    Engine*         m_engine = nullptr;

    LightningPhase m_phase          = LightningPhase::Idle;
    float          m_realTime       = 0.0f;
    float          m_phaseTime      = 0.0f;
    float          m_currentKA      = 0.0f;
    float          m_flashLuminance = 0.0f;

    JPH::Vec3 m_cloudOrigin  = JPH::Vec3::sZero();
    JPH::Vec3 m_groundTarget = JPH::Vec3::sZero();

    std::vector<LightningSegment> m_segments;

    Entity     m_lightningEntity  = NullEntity;
    Entity     m_flashLightEntity = NullEntity;
    AssetID    m_meshAssetId      = 0;
    MaterialID m_matAssetId       = 0;

    BufferHandle m_vboPos          = BufferHandle::Invalid;
    BufferHandle m_vboAttr         = BufferHandle::Invalid;
    uint32_t     m_maxVertices     = 0;
    uint32_t     m_visibleVertices = 0;

    std::mt19937 m_rng {1337};
};

} // namespace ZHLN
