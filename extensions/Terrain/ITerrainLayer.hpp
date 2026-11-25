// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Binary-stable secondary COM interface exposed by libzahlen_plugin_terrain
// alongside COM::IAppLayer. Hosts that load the plugin dynamically obtain this
// interface via `layer.cast<ZHLN::Terrain::ITerrainLayer>()` without linking
// the static zahlen_terrain library.

#include "TerrainTypes.hpp"
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Terrain {

// Optional COM::InChain payload accepted by libzahlen_plugin_terrain's
// CreateAppLayerRaw entry point.
struct PluginConfig {
    bool enabled = true; // when false, OnBeforeRender is a no-op
};

static_assert(std::is_standard_layout_v<PluginConfig> && std::is_trivially_copyable_v<PluginConfig>);

struct ITerrainLayer : COM::IObject {
    // Generate a procedural heightfield tile and attach it to the live engine
    // bound during IAppLayer::OnStart. Returns Terrain::Error::NotStarted if
    // called before OnStart or after OnStop.
    [[nodiscard]] virtual COM::Result<Entity> GenerateTile(
        const GenerateInfo& generate,
        const CreateInfo&   create
    ) noexcept = 0;

    // Build a terrain tile from caller-supplied height samples
    // (sampleCount * sampleCount floats) and optional RGBA colors
    // (sampleCount * sampleCount * 4 floats, or empty for procedural coloring).
    [[nodiscard]] virtual COM::Result<Entity> CreateTileFromHeights(
        uint32_t               sampleCount,
        float                  worldSize,
        COM::Span<const float> heights,
        COM::Span<const float> colorsRGBA,
        const CreateInfo&      create
    ) noexcept = 0;

    [[nodiscard]] virtual float    SampleHeightAt(float worldX, float worldZ) const noexcept = 0;
    [[nodiscard]] virtual uint64_t TileCount() const noexcept                                = 0;
    virtual void                   RemoveTile(Entity entity) noexcept                        = 0;
    virtual void                   ClearTiles() noexcept                                     = 0;

    // Ergonomic inline defaults (compiled in the consumer TU, never on the wire).
    [[nodiscard]] auto GenerateTile(const GenerateInfo& generate = {}) noexcept -> COM::Result<Entity> {
        return GenerateTile(generate, CreateInfo {});
    }

  protected:
    ~ITerrainLayer() = default;
};

static_assert(COM::AuditInterfaceABI<ITerrainLayer>(), "ITerrainLayer violates the COM ABI rules.");

} // namespace ZHLN::Terrain
