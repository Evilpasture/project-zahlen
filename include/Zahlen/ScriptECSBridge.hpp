// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ScriptBinder.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

class ZHLN_API ScriptECSBridge {
  public:
    explicit ScriptECSBridge(ECS::Registry& reg): m_registry(reg) {
    }

    /**
     * @brief Registers all nested component structs inside a Manifest type.
     * Pure C++ typename signature — ZERO reflection tokens!
     */
    template <typename Manifest>
    void RegisterComponentManifest() {
        // 1. Register with ScriptBinder
        ZHLN::RegisterManifest<Manifest>();

        // 2. Register with ECS Registry
        ZHLN::Reflect::ForEachNestedType<Manifest>([this]<typename Comp>() { m_registry.RegisterComponent<Comp>(ZHLN::Reflect::TypeName<Comp>()); });
    }

    // --- Entity Component Roots ---
    [[nodiscard]] std::expected<ScriptVal, Error> GetProperty(Entity entity, std::string_view compName, std::string_view propName);
    [[nodiscard]] std::expected<void, Error>      SetProperty(Entity entity, std::string_view compName, std::string_view propName, const ScriptVal& val);
    [[nodiscard]] std::expected<ScriptVal, Error>
        CallMethod(Entity entity, std::string_view compName, std::string_view methodName, std::span<const ScriptVal> args = {});

    // --- Multi-Level Object Drilling ---
    [[nodiscard]] std::expected<ScriptVal, Error> GetPropertyOf(const ScriptVal& parentVal, std::string_view propName);
    [[nodiscard]] std::expected<void, Error>      SetPropertyOf(ScriptVal& parentVal, std::string_view propName, const ScriptVal& val);

    // --- Container Array Operations ---
    [[nodiscard]] std::expected<ScriptVal, Error> GetArrayElement(const ScriptVal& arrayVal, size_t index);
    [[nodiscard]] std::expected<void, Error>      SetArrayElement(ScriptVal& arrayVal, size_t index, const ScriptVal& val);

    [[nodiscard]] std::expected<ScriptVal, Error> GetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index);
    [[nodiscard]] std::expected<void, Error>
        SetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index, const ScriptVal& val);

    [[nodiscard]] std::expected<void*, Error> ResolveBoxedPointer(const BoxedObject& obj) const;

  private:
    ECS::Registry& m_registry;
};

} // namespace ZHLN
