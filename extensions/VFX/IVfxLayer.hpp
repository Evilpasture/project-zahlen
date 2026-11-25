// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable secondary COM interface exposed by libzahlen_plugin_vfx
// alongside COM::IAppLayer. Hosts that load the plugin dynamically obtain this
// interface via `layer.cast<ZHLN::VFX::IVfxLayer>()` without linking the
// static zahlen_vfx library.

#include "VFXTypes.hpp"
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::VFX {

// Optional COM::InChain payload accepted by libzahlen_plugin_vfx's
// CreateAppLayerRaw entry point.
struct PluginConfig {
    bool enabled = true; // when false, OnBeforeRender is a no-op
};

static_assert(std::is_standard_layout_v<PluginConfig> && std::is_trivially_copyable_v<PluginConfig>);

struct IVfxLayer : COM::IObject {
    [[nodiscard]] virtual COM::Result<Entity> SpawnExplosion(const Explosion::SpawnInfo& info) noexcept = 0;
    virtual void                              RemoveExplosion(Entity entity) noexcept                   = 0;
    virtual void                              ClearExplosions() noexcept                                = 0;
    [[nodiscard]] virtual uint64_t            ExplosionCount() const noexcept                           = 0;
    [[nodiscard]] virtual uint64_t            CraterCount() const noexcept                              = 0;

    [[nodiscard]] virtual COM::Result<Entity> SpawnLightning(
        FVec3                        cloud,
        FVec3                        ground,
        FVec3                        camera,
        const Lightning::ConfigInfo& config
    ) noexcept = 0;
    virtual void                   RemoveLightning(Entity entity) noexcept = 0;
    virtual void                   ClearLightning() noexcept               = 0;
    [[nodiscard]] virtual uint64_t LightningCount() const noexcept         = 0;

    // Ergonomic inline defaults (compiled in the consumer TU, never on the wire).
    [[nodiscard]] auto SpawnLightning(FVec3 cloud, FVec3 ground, FVec3 camera) noexcept -> COM::Result<Entity> {
        return SpawnLightning(cloud, ground, camera, Lightning::ConfigInfo {});
    }

  protected:
    ~IVfxLayer() = default;
};

static_assert(COM::AuditInterfaceABI<IVfxLayer>(), "IVfxLayer violates the COM ABI rules.");

} // namespace ZHLN::VFX
