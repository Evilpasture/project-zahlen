// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "VFXTypes.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <expected>
#include <memory>
namespace ZHLN::Explosion {
struct EmitterState {
    float    age           = 0;
    uint32_t fireParticles = 0, smokeParticles = 0, shockwaves = 0;
};
struct CraterState {
    float age = 0, lifetime = 28;
};
class System {
  public:
    System();
    ~System();
    System(System&&) noexcept;
    auto operator=(System&&) noexcept -> System&;
    System(const System&)                    = delete;
    auto operator=(const System&) -> System& = delete;
    auto Start(COM::IEngine&) -> std::expected<void, ErrorCode>;
    auto Spawn(const SpawnInfo&) -> std::expected<Entity, ErrorCode>;
    auto BeforeRender(COM::IEngine&, float) -> std::expected<void, ErrorCode>;
    void Remove(Entity);
    void Clear();
    void Stop(COM::IEngine&);
    auto Size() const noexcept -> size_t;
    auto CraterCount() const noexcept -> size_t;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
} // namespace ZHLN::Explosion
