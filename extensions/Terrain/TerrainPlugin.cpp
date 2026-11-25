// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Dynamically loadable COM::IAppLayer + Terrain::ITerrainLayer plugin. Wraps
// ZHLN::Terrain::System so a host can start, generate, query, render, and stop
// tile-based terrain across the standard plugin ABI (CreateAppLayerRaw +
// EntryPointWitness::Seal) without linking the static zahlen_terrain library.
//
// Configuration arrives through a ChainInNode payload (Terrain::PluginConfig);
// an absent or empty chain means default configuration.

#include <Terrain/ITerrainLayer.hpp>
#include <Terrain/Terrain.hpp>

#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Contract.hpp>
#include <new>
#include <span>

namespace {

using namespace ZHLN;

class TerrainPlugin final : public COM::Implements<TerrainPlugin, COM::IAppLayer, Terrain::ITerrainLayer> {
  public:
    explicit TerrainPlugin(Terrain::PluginConfig config) noexcept
        : _config(config) {}

    // --- COM::IAppLayer ------------------------------------------------------

    auto OnStart(COM::IEngine* engine) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        _engine = engine;
        _terrain.Start(*engine);
        return {};
    }

    auto OnTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    auto OnBeforeRender(COM::IEngine* engine, float dt) noexcept -> COM::Result<void> override {
        if (engine == nullptr) {
            return COM::Error::NullArgument;
        }
        if (!_config.enabled) {
            return {};
        }
        const auto result = _terrain.BeforeRender(*engine, dt);
        if (!result) {
            return result.error();
        }
        return {};
    }

    auto OnAfterTick(COM::IEngine*, float) noexcept -> COM::Result<void> override {
        return {};
    }

    void OnStop(COM::IEngine* engine) noexcept override {
        _engine = nullptr;
        if (engine != nullptr) {
            _terrain.Stop(*engine);
        } else {
            _terrain.Clear();
        }
    }

    // --- Terrain::ITerrainLayer ----------------------------------------------

    auto GenerateTile(const Terrain::GenerateInfo& generate, const Terrain::CreateInfo& create) noexcept
        -> COM::Result<Entity> override {
        if (_engine == nullptr) {
            return Terrain::Error::NotStarted;
        }
        auto geometry = Terrain::Generate(generate);
        if (!geometry) {
            return geometry.error();
        }
        const auto created = _terrain.Create(std::move(*geometry), create);
        if (!created) {
            return created.error();
        }
        return *created;
    }

    auto CreateTileFromHeights(
        uint32_t                  sampleCount,
        float                     worldSize,
        COM::Span<const float>    heights,
        COM::Span<const float>    colorsRGBA,
        const Terrain::CreateInfo& create
    ) noexcept -> COM::Result<Entity> override {
        if (_engine == nullptr) {
            return Terrain::Error::NotStarted;
        }
        if ((heights.size > 0 && heights.data == nullptr) || (colorsRGBA.size > 0 && colorsRGBA.data == nullptr)) {
            return COM::Error::NullArgument;
        }
        auto geometry = Terrain::FromHeights(
            sampleCount,
            worldSize,
            std::span<const float>(heights.data, heights.size),
            std::span<const float>(colorsRGBA.data, colorsRGBA.size)
        );
        if (!geometry) {
            return geometry.error();
        }
        const auto created = _terrain.Create(std::move(*geometry), create);
        if (!created) {
            return created.error();
        }
        return *created;
    }

    auto SampleHeightAt(float worldX, float worldZ) const noexcept -> float override {
        return _terrain.SampleHeightAt(worldX, worldZ);
    }

    auto TileCount() const noexcept -> uint64_t override {
        return static_cast<uint64_t>(_terrain.Size());
    }

    void RemoveTile(Entity entity) noexcept override {
        _terrain.Remove(entity);
    }

    void ClearTiles() noexcept override {
        _terrain.Clear();
        if (_engine != nullptr) {
            _terrain.Start(*_engine);
        }
    }

  private:
    COM::IEngine*         _engine = nullptr;
    Terrain::System       _terrain {};
    Terrain::PluginConfig _config {};
};

} // namespace

namespace ZHLN::COM {

ZHLN_EXPORT auto CreateAppLayerRaw(const ChainInNode* config) noexcept -> Result<IAppLayer*> {
    ZHLN::Terrain::PluginConfig parsed {};
    const auto                  checked = DispatchInChain<ZHLN::Terrain::PluginConfig>(
        config, [&](const ZHLN::Terrain::PluginConfig& value) noexcept { parsed = value; });
    if (!checked.HasValue()) {
        return checked.error;
    }
    auto* instance = new (std::nothrow) TerrainPlugin(parsed);
    if (instance == nullptr) {
        return Error::AllocationFailed;
    }
    return static_cast<IAppLayer*>(instance);
}

} // namespace ZHLN::COM

template <>
ZHLN_EXPORT void ZHLN::COM::EntryPointWitness<&ZHLN::COM::CreateAppLayerRaw>::Seal() noexcept {}
