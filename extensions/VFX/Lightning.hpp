// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LightningGeometry.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <expected>
#include <memory>
namespace ZHLN::Lightning {
enum class Phase : uint8_t { SteppedLeader, ReturnStroke, Dissipating };
// POD diagnostics; no owning GPU handles or registry access.
struct BoltState {
    Phase    phase     = Phase::SteppedLeader;
    float    phaseTime = 0, currentKA = 0, flashLuminance = 0;
    uint32_t visibleVertices = 0, maxVertices = 0;
};
class System {
  public:
    System();
    ~System();
    System(System&&) noexcept;
    auto operator=(System&&) noexcept -> System&;
    System(const System&)                    = delete;
    auto operator=(const System&) -> System& = delete;
    void Start(COM::IEngine&);
    auto Spawn(FVec3 cloud, FVec3 ground, FVec3 camera, const ConfigInfo& = {}) -> std::expected<Entity, ErrorCode>;
    auto BeforeRender(COM::IEngine&, float) -> std::expected<void, ErrorCode>;
    void Remove(Entity);
    void Clear();
    void Stop(COM::IEngine&);
    auto Size() const noexcept -> size_t;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace ZHLN::Lightning
