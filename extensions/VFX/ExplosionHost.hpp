// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <expected>
#include <memory>
namespace ZHLN::ExplosionHost {
enum class Kind : uint8_t { StandardFireball, ArtilleryMortar };
struct SpawnInfo {
    FVec3 origin {};
    float scale = 1;
    Kind  kind  = Kind::ArtilleryMortar;
};
struct EmitterState {
    float    age           = 0;
    uint32_t fireParticles = 0, smokeParticles = 0, shockwaves = 0;
};
struct CraterState {
    float age = 0, lifetime = 28;
};
class Host {
  public:
    Host();
    ~Host();
    Host(Host&&) noexcept;
    auto operator=(Host&&) noexcept -> Host&;
    Host(const Host&)                    = delete;
    auto operator=(const Host&) -> Host& = delete;
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
} // namespace ZHLN::ExplosionHost
