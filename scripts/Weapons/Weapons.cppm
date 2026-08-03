// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

export module ZHLN.Weapons;

import ZHLN.MathUtils;

export namespace ZHLN::Weapons {

enum class WeaponId : uint8_t { Rifle = 0, Shotgun = 1, Minigun = 2, Count = 3 };

struct WeaponDef {
    WeaponId    id               = WeaponId::Rifle;
    std::string name             = "MK-18";
    std::string caliber          = "5.56";
    uint32_t    pellets          = 1;
    float       damage           = 30.0f;
    int32_t     magSize          = 30;
    int32_t     startReserve     = 210;
    float       fireRate         = 0.086f;
    float       reloadTime       = 2.1f;
    float       shellReload      = 0.0f;
    float       baseSpread       = 0.005f;
    float       patternSpread    = 0.0f;
    float       adsTighten       = 0.8f;
    float       falloffStart     = 45.0f;
    float       falloffEnd       = 110.0f;
    float       falloffMin       = 0.55f;
    float       range            = 220.0f;
    float       recoilPitch      = 0.55f;
    float       recoilYaw        = 0.5f;
    float       kick             = 1.9f;
    float       adsFov           = 42.0f;
    uint32_t    tracerColor      = 0xfff0b0;
    float       flashScale       = 0.75f;
    float       spinUp           = 0.0f;
    float       moveScale        = 1.0f;
    bool        godOnly          = false;
    bool        noAds            = false;
    float       penetration      = 1.35f;
    float       bodyPenRetain    = 0.55f;
    float       surfacePenRetain = 0.72f;
    uint32_t    maxBodyPierces   = 2;
};

inline const std::array<WeaponDef, static_cast<size_t>(WeaponId::Count)> WEAPON_DEFS = {
    {{.id               = WeaponId::Rifle,
      .name             = "MK-18",
      .caliber          = "5.56",
      .pellets          = 1,
      .damage           = 30.0f,
      .magSize          = 30,
      .startReserve     = 210,
      .fireRate         = 0.086f,
      .reloadTime       = 2.1f,
      .shellReload      = 0.0f,
      .baseSpread       = 0.005f,
      .patternSpread    = 0.0f,
      .adsTighten       = 0.8f,
      .falloffStart     = 45.0f,
      .falloffEnd       = 110.0f,
      .falloffMin       = 0.55f,
      .range            = 220.0f,
      .recoilPitch      = 0.55f,
      .recoilYaw        = 0.5f,
      .kick             = 1.9f,
      .adsFov           = 42.0f,
      .tracerColor      = 0xfff0b0,
      .flashScale       = 0.75f,
      .spinUp           = 0.0f,
      .moveScale        = 1.0f,
      .godOnly          = false,
      .noAds            = false,
      .penetration      = 1.35f,
      .bodyPenRetain    = 0.55f,
      .surfacePenRetain = 0.72f,
      .maxBodyPierces   = 2},
     {.id               = WeaponId::Shotgun,
      .name             = "M870 BREACHER",
      .caliber          = "12GA",
      .pellets          = 9,
      .damage           = 16.0f,
      .magSize          = 7,
      .startReserve     = 70,
      .fireRate         = 0.62f,
      .reloadTime       = 0.0f,
      .shellReload      = 0.45f,
      .baseSpread       = 0.012f,
      .patternSpread    = 0.05f,
      .adsTighten       = 0.7f,
      .falloffStart     = 11.0f,
      .falloffEnd       = 40.0f,
      .falloffMin       = 0.2f,
      .range            = 60.0f,
      .recoilPitch      = 2.1f,
      .recoilYaw        = 0.9f,
      .kick             = 5.5f,
      .adsFov           = 52.0f,
      .tracerColor      = 0xffc070,
      .flashScale       = 1.35f,
      .spinUp           = 0.0f,
      .moveScale        = 1.0f,
      .godOnly          = false,
      .noAds            = false,
      .penetration      = 0.0f,
      .bodyPenRetain    = 0.0f,
      .surfacePenRetain = 0.0f,
      .maxBodyPierces   = 0},
     {.id               = WeaponId::Minigun,
      .name             = "M134 VULCAN",
      .caliber          = "7.62 BELT",
      .pellets          = 1,
      .damage           = 17.0f,
      .magSize          = 600,
      .startReserve     = 2400,
      .fireRate         = 0.028f,
      .reloadTime       = 5.5f,
      .shellReload      = 0.0f,
      .baseSpread       = 0.03f,
      .patternSpread    = 0.0f,
      .adsTighten       = 0.0f,
      .falloffStart     = 40.0f,
      .falloffEnd       = 120.0f,
      .falloffMin       = 0.5f,
      .range            = 190.0f,
      .recoilPitch      = 0.11f,
      .recoilYaw        = 0.16f,
      .kick             = 0.45f,
      .adsFov           = 70.0f,
      .tracerColor      = 0xffdd66,
      .flashScale       = 1.15f,
      .spinUp           = 0.62f,
      .moveScale        = 0.55f,
      .godOnly          = true,
      .noAds            = true,
      .penetration      = 3.6f,
      .bodyPenRetain    = 0.72f,
      .surfacePenRetain = 0.85f,
      .maxBodyPierces   = 4}}
};

[[nodiscard]] inline const WeaponDef& GetWeaponDef(WeaponId id) noexcept {
    return WEAPON_DEFS[static_cast<size_t>(id)];
}

[[nodiscard]] inline float FalloffAt(const WeaponDef& def, float dist) noexcept {
    if (dist <= def.falloffStart)
        return 1.0f;
    if (dist >= def.falloffEnd)
        return def.falloffMin;
    float t = (dist - def.falloffStart) / (def.falloffEnd - def.falloffStart);
    return 1.0f + (def.falloffMin - 1.0f) * t;
}

struct ProceduralWeaponMeshBuilder {
    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<uint32_t>         indices;

    void AddBox(JPH::Vec3Arg center, JPH::Vec3Arg size, JPH::Vec4Arg color, JPH::QuatArg rot = JPH::Quat::sIdentity()) {
        JPH::Vec3 hs = size * 0.5f;

        struct Face {
            JPH::Vec3 n, u, v;
        };
        std::array<Face, 6> faces = {{
            {JPH::Vec3(0, 0, 1), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 0, -1), JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 0, 1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1)},
            {JPH::Vec3(0, -1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, 1)},
        }};

        PackedRGBA8 col = Math::PackColor(color.GetX(), color.GetY(), color.GetZ(), color.GetW());

        for (const auto& f: faces) {
            uint32_t      fBase      = static_cast<uint32_t>(positions.size());
            JPH::Vec3     norm       = rot * f.n;
            Packed1010102 packedNorm = Math::PackNormal(norm.GetX(), norm.GetY(), norm.GetZ());
            Packed1010102 packedTang = Math::PackNormal(1.0f, 0.0f, 0.0f);

            std::array<std::pair<float, float>, 4> uvs = {{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
            for (const auto& [su, sv]: uvs) {
                JPH::Vec3 offset = f.n * hs + (f.u * hs) * su + (f.v * hs) * sv;
                JPH::Vec3 pos    = center + rot * offset;

                positions.push_back({{pos.GetX(), pos.GetY(), pos.GetZ()}});
                attributes.push_back({.normal = packedNorm, .tangent = packedTang, .uv = Math::PackUV(0.0f, 0.0f), .color = col});
            }

            indices.push_back(fBase);
            indices.push_back(fBase + 1);
            indices.push_back(fBase + 2);
            indices.push_back(fBase);
            indices.push_back(fBase + 2);
            indices.push_back(fBase + 3);
        }
    }

    Mesh Build(RenderContext& rc) {
        Mesh mesh;
        mesh.posBuffer   = rc.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
        mesh.attrBuffer  = rc.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));
        mesh.indexBuffer = rc.CreateIndexBuffer(indices.data(), indices.size() * sizeof(uint32_t));
        mesh.vertexCount = static_cast<uint32_t>(positions.size());
        mesh.indexCount  = static_cast<uint32_t>(indices.size());
        return mesh;
    }
};

AssetID GetOrGenerateWeaponMeshAsset(RenderContext& rc, WeaponId id) {
    std::string assetName = std::format("procedural_weapon_single_mesh_{}", static_cast<uint32_t>(id));
    AssetID     assetId   = HashAssetID(assetName);

    if (!rc.GetGPUMesh(assetId).has_value()) {
        ProceduralWeaponMeshBuilder builder;
        JPH::Vec4                   metalCol(0.20f, 0.22f, 0.24f, 1.0f);
        JPH::Vec4                   woodCol(0.40f, 0.25f, 0.12f, 1.0f);
        float                       scale = 0.85f;

        if (id == WeaponId::Shotgun) {
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.040f) * scale, JPH::Vec3(0.062f, 0.095f, 0.400f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.036f, 0.440f) * scale, JPH::Vec3(0.042f, 0.042f, 0.560f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.014f, 0.400f) * scale, JPH::Vec3(0.036f, 0.036f, 0.460f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.012f, 0.350f) * scale, JPH::Vec3(0.062f, 0.062f, 0.190f) * scale, woodCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.082f, -0.280f) * scale, JPH::Vec3(0.050f, 0.100f, 0.070f) * scale, woodCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.008f, -0.260f) * scale, JPH::Vec3(0.055f, 0.100f, 0.280f) * scale, woodCol);
        } else if (id == WeaponId::Minigun) {
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.500f) * scale, JPH::Vec3(0.120f, 0.120f, 0.780f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, -0.060f) * scale, JPH::Vec3(0.200f, 0.216f, 0.340f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, -0.040f, -0.260f) * scale, JPH::Vec3(0.160f, 0.140f, 0.260f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(-0.020f, -0.200f, -0.340f) * scale, JPH::Vec3(0.260f, 0.260f, 0.160f) * scale, metalCol);
        } else { // Rifle
            builder.AddBox(JPH::Vec3(0.000f, 0.000f, 0.060f) * scale, JPH::Vec3(0.058f, 0.088f, 0.460f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.010f, 0.420f) * scale, JPH::Vec3(0.034f, 0.034f, 0.340f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.010f, 0.620f) * scale, JPH::Vec3(0.044f, 0.044f, 0.060f) * scale, metalCol);
            builder.AddBox(
                JPH::Vec3(0.000f, -0.110f, 0.020f) * scale, JPH::Vec3(0.048f, 0.160f, 0.088f) * scale, metalCol,
                JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.25f)
            );
            builder.AddBox(JPH::Vec3(0.000f, -0.080f, -0.300f) * scale, JPH::Vec3(0.044f, 0.100f, 0.070f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.004f, -0.240f) * scale, JPH::Vec3(0.048f, 0.082f, 0.240f) * scale, metalCol);
            builder.AddBox(JPH::Vec3(0.000f, 0.050f, 0.125f) * scale, JPH::Vec3(0.026f, 0.038f, 0.055f) * scale, metalCol);
        }

        Mesh mesh = builder.Build(rc);
        rc.RegisterGPUMesh(assetId, mesh);
    }
    return assetId;
}

Entity CreateWeaponModel(Engine* engine, WeaponId id, MaterialID metalMat, MaterialID woodMat) {
    auto& reg = engine->GetRegistry();
    auto& rc  = engine->GetRenderContext();

    AssetID meshAsset = GetOrGenerateWeaponMeshAsset(rc, id);

    Entity weapon = reg.Create();
    reg.Add(weapon, Components::TransformComponent {});
    reg.Add(weapon, Components::NameComponent {.name = String64("ProceduralWeapon")});
    reg.Add(weapon, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = (id == WeaponId::Shotgun) ? woodMat : metalMat, .cullRadius = 2.0f});

    return weapon;
}

} // namespace ZHLN::Weapons
