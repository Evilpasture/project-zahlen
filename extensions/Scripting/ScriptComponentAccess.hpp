// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/ECS/ComponentPolicy.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
namespace ZHLN {
// Consumer-owned reflected names -> ABI-published POD storage. This retains the
// capability owner, never an Engine/Registry/Inspection. All pointers are used
// synchronously at a host phase boundary, never across structural mutations.
class ScriptComponentAccess {
  public:
    explicit ScriptComponentAccess(COM::IEntities& entities): m_entities(&entities), m_scope(std::make_shared<COM::Ref<COM::IEntities>>(m_entities)) {
    }
    template <class T>
    void Register(bool requirePublished = false, bool writable = !SceneResources::ReadOnlyReflective<T>) {
        if constexpr (COM::Component<T>) {
            constexpr auto canonical = Reflect::CanonicalTypeName<T>();
            if (requirePublished && !m_entities->IsEngineComponent(canonical))
                return;
            Binding b {
                +[](COM::IEntities& e, Entity entity) -> void* { return e.Get<T>(entity); },
                +[](const void* p, std::string& out) { out += Reflect::ToDebugString(*static_cast<const T*>(p)); }, writable
            };
            m_bindings[std::string(Reflect::TypeName<T>())] = b;
            m_bindings[std::string(canonical)]              = b;
        }
    }
    template <class Manifest>
    void RegisterManifest(bool requirePublished = false) {
        Reflect::ForEachNestedType<Manifest>([&]<class T>() { Register<T>(requirePublished); });
    }
    bool IsAlive(Entity e) const noexcept {
        return m_entities->IsAlive(e);
    }
    bool IsRegistered(std::string_view name) const {
        return m_bindings.contains(std::string(name));
    }
    void* Component(Entity e, std::string_view name) const {
        if (!IsAlive(e))
            return nullptr;
        auto it = m_bindings.find(std::string(name));
        return it == m_bindings.end() ? nullptr : it->second.get(*m_entities.Get(), e);
    }
    bool CanWrite(std::string_view name) const {
        auto it = m_bindings.find(std::string(name));
        return it != m_bindings.end() && it->second.writable;
    }
    void Dump(Entity e, std::string& out) const {
        if (!IsAlive(e))
            return;
        for (const auto& [name, binding]: m_bindings) {
            if (name.find("::") != std::string::npos)
                continue; // no duplicate canonical aliases
            if (auto* p = binding.get(*m_entities.Get(), e)) {
                out += name;
                out += ": ";
                binding.dump(p, out);
                out += '\n';
            }
        }
    }
    template <COM::Component T>
    auto EntitiesWith() const {
        return m_entities->EntitiesWith<T>();
    }
    template <COM::Component T>
    auto Read(Entity e) const -> const T* {
        return m_entities->Get<T>(e);
    }
    auto Scope() const noexcept -> std::shared_ptr<void> {
        return m_scope;
    }
    auto Entities() const noexcept -> COM::IEntities& {
        return *m_entities.Get();
    }

  private:
    struct Binding {
        void* (*get)(COM::IEntities&, Entity);
        void (*dump)(const void*, std::string&);
        bool writable;
    };
    COM::Ref<COM::IEntities>                 m_entities;
    std::shared_ptr<void>                    m_scope;
    std::unordered_map<std::string, Binding> m_bindings;
};
} // namespace ZHLN
