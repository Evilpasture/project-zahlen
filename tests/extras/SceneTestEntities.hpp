// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Scene/Scene.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <algorithm>
#include <tuple>
// Test-only read capability over fixture storage. No registry crosses into Scene.
class SceneTestEntities final: public ZHLN::COM::IEntities {
    const ZHLN::ECS::Registry& registry;
    template <typename F>
    auto Visit(ZHLN::COM::StringView name, F&& fn) const {
        using C    = ZHLN::Components;
        bool found = false;
        [&]<typename... T>(std::tuple<T...>*) {
            ((std::string_view(name) == std::string_view(ComponentName<T>()) ? (fn.template operator()<T>(), found = true, 0) : 0), ...);
        }(static_cast<std::tuple<
              C::TransformComponent, C::NameComponent, C::MeshComponent, C::SceneSourceComponent, C::PBRComponent, C::PhysicsComponent, C::LightComponent,
              C::SceneLightTagComponent, C::AmbientCubeComponent, C::PendingDestroy, ZHLN::Scene::AuthoringComponent>*>(nullptr));
        return found;
    }

  public:
    explicit SceneTestEntities(const ZHLN::ECS::Registry& r): registry(r) {
    }
    uint32_t AddRef() noexcept override {
        return 1;
    }
    uint32_t Release() noexcept override {
        return 1;
    }
    void* QueryRaw(ZHLN::COM::InterfaceID) noexcept override {
        return nullptr;
    }
    ZHLN::Entity CreateEntity() noexcept override {
        return ZHLN::Entity::Null();
    }
    void Despawn(ZHLN::Entity) noexcept override {
    }
    bool IsAlive(ZHLN::Entity e) const noexcept override {
        return registry.IsAlive(e);
    }
    void* AttachRaw(ZHLN::Entity, ZHLN::COM::StringView, uint32_t, uint32_t) noexcept override {
        return nullptr;
    }
    bool DetachRaw(ZHLN::Entity, ZHLN::COM::StringView) noexcept override {
        return false;
    }
    void* GetRaw(ZHLN::Entity e, ZHLN::COM::StringView name) const noexcept override {
        void* value = nullptr;
        Visit(name, [&]<typename T>() {
            const auto v = registry.Get<T>(e);
            if (v)
                value = const_cast<T*>(&*v);
        });
        return value;
    }
    bool IsEngineComponent(ZHLN::COM::StringView name) const noexcept override {
        return Visit(name, []<typename>() {});
    }
    void* AttachEngineRaw(ZHLN::Entity, ZHLN::COM::StringView, uint32_t, uint32_t, uint64_t) noexcept override {
        return nullptr;
    }
    bool DetachEngineRaw(ZHLN::Entity, ZHLN::COM::StringView) noexcept override {
        return false;
    }
    void* GetEngineRaw(ZHLN::Entity e, ZHLN::COM::StringView name) const noexcept override {
        return GetRaw(e, name);
    }
    uint32_t EntitiesWithRaw(ZHLN::COM::StringView name, ZHLN::Entity* out, uint32_t cap) const noexcept override {
        uint32_t count = 0;
        Visit(name, [&]<typename T>() {
            const auto ids = registry.GetEntitiesWith<T>();
            count          = static_cast<uint32_t>(ids.size());
            if (out)
                std::copy_n(ids.begin(), std::min(cap, count), out);
        });
        return count;
    }
};
