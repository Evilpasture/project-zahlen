// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Engine/Capabilities/Entities.hpp
//
// Engine's ECS capability. Unlike the other facades this one brings the
// component vocabulary with it: its operations are templates typed by the
// caller's component, so the ECS declarations have to be visible here.
// Engine.hpp itself stays light -- it only forward-declares Entities.
//
// Entities is deliberately NOT a 1:1 pass-through to Registry. Any mutation
// that can retire an external resource (Jolt body, miniaudio voice, Vulkan
// buffer) goes through SceneResources first, and destruction is deferred via
// DespawnEntity so SceneCleanup can release those resources while the
// components are still readable. Raw Registry::Destroy / Add / Remove do
// neither, which is why Registry is not part of Engine's public API.

#include <Zahlen/Common.h>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <utility>

namespace ZHLN {

class Engine;

class ZHLN_API Entities {
  public:
    // --- Safe entity lifecycle -------------------------------------------------
    [[nodiscard]] auto Create() -> Entity;

    template <typename... Comps>
    auto Spawn(Comps&&... comps) -> Entity {
        return Registry().Create(std::forward<Comps>(comps)...);
    }

    // Marks the hierarchy PendingDestroy; SceneCleanup releases physics bodies,
    // audio voices and GPU buffers before the registry erases the entity.
    void               Despawn(Entity entity) noexcept;
    [[nodiscard]] auto IsAlive(Entity entity) const noexcept -> bool;

    // --- Read-only queries -----------------------------------------------------
    template <typename T>
    [[nodiscard]] auto Get(Entity entity) const noexcept -> ZHLN::Optional<const T&> {
        return std::as_const(Registry()).Get<T>(entity);
    }

    template <typename T>
    [[nodiscard]] auto GetSingleton() const noexcept -> ZHLN::Optional<const T&> {
        return std::as_const(Registry()).GetSingleton<T>();
    }

    // --- Component attachment / detachment -------------------------------------
    // Replaced or removed components release their external resource first.
    template <typename T>
    auto Attach(Entity entity, T component) -> T& {
        if constexpr (SceneResources::OwnsExternalResource<T>) {
            SceneResources::Detach<T, Engine>(*_engine, Registry(), entity);
        } else {
            Registry().Remove<T>(entity);
        }
        return Registry().Add(entity, std::move(component));
    }

    template <typename T>
    auto Detach(Entity entity) -> bool {
        if constexpr (SceneResources::OwnsExternalResource<T>) {
            return SceneResources::Detach<T, Engine>(*_engine, Registry(), entity);
        }
        auto& registry = Registry();
        if (!registry.Get<T>(entity)) {
            return false;
        }
        registry.Remove<T>(entity);
        return true;
    }

  private:
    friend class Engine;
    explicit Entities(Engine& engine) noexcept: _engine(&engine) {
    }

    // Out-of-line so this installed header never reaches the engine's PIMPL.
    [[nodiscard]] auto Registry() const noexcept -> ECS::Registry&;

    Engine* _engine;
};

} // namespace ZHLN
