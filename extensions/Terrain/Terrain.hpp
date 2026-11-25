// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainGeometry.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <expected>
#include <memory>
namespace ZHLN::Terrain {
// Host-owned configuration, not a handle into the native global slot table.
struct TileComponent {
    float roughness = 0.85f;
    float metallic  = 0.05f;
};
// Instance-scoped CPU source documents survive cache clears and GPU recovery.
// Drive from Application: Start -> Create/Generate -> BeforeRender -> Stop.
// Authoring/physics body creation happen before AdvanceFrame, never in a graph.
class System {
  public:
    System();
    ~System();
    System(System&&) noexcept;
    auto operator=(System&&) noexcept -> System&;
    System(const System&)                                  = delete;
    auto               operator=(const System&) -> System& = delete;
    void               Start(COM::IEngine&);
    [[nodiscard]] auto Create(Geometry geometry, const CreateInfo& info = {}) -> std::expected<Entity, ErrorCode>;
    [[nodiscard]] auto BeforeRender(COM::IEngine&, float) -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto SampleHeightAt(float worldX, float worldZ) const noexcept -> float;
    [[nodiscard]] auto Size() const noexcept -> size_t;
    void               Remove(Entity);
    void               Clear();
    void               Stop(COM::IEngine&);

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace ZHLN::Terrain
