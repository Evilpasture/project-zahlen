// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "LightningGeometry.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <expected>
#include <memory>
namespace ZHLN::LightningHost {
enum class Phase : uint8_t { SteppedLeader, ReturnStroke, Dissipating };
// POD diagnostics; no owning GPU handles or registry access.
struct BoltState {
    Phase    phase     = Phase::SteppedLeader;
    float    phaseTime = 0, currentKA = 0, flashLuminance = 0;
    uint32_t visibleVertices = 0, maxVertices = 0;
};
class Host {
  public:
    Host();
    ~Host();
    Host(Host&&) noexcept;
    auto operator=(Host&&) noexcept -> Host&;
    Host(const Host&)                    = delete;
    auto operator=(const Host&) -> Host& = delete;
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
} // namespace ZHLN::LightningHost
