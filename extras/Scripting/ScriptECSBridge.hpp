// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Scripting/ScriptBinder.hpp>
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
        ZHLN::Reflect::ForEachNestedType<Manifest>([this]<typename Comp>() -> auto { m_registry.RegisterComponent<Comp>(ZHLN::Reflect::TypeName<Comp>()); });
    }

    // --- Entity Component Roots ---
    [[nodiscard]] auto GetProperty(Entity entity, std::string_view compName, std::string_view propName) -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetProperty(Entity entity, std::string_view compName, std::string_view propName, const ScriptVal& val) -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto CallMethod(Entity entity, std::string_view compName, std::string_view methodName, std::span<const ScriptVal> args = {})
        -> std::expected<ScriptVal, ErrorCode>;

    // --- Multi-Level Object Drilling ---
    [[nodiscard]] auto GetPropertyOf(const ScriptVal& parentVal, std::string_view propName) const -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetPropertyOf(ScriptVal& parentVal, std::string_view propName, const ScriptVal& val) const -> std::expected<void, ErrorCode>;

    // --- Container Array Operations ---
    [[nodiscard]] auto GetArrayElement(const ScriptVal& arrayVal, size_t index) -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetArrayElement(ScriptVal& arrayVal, size_t index, const ScriptVal& val) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto
        GetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index) -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index, const ScriptVal& val)
        -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto ResolveBoxedPointer(const BoxedObject& obj) const -> std::expected<void*, ErrorCode>;

  private:
    ECS::Registry& m_registry;
};

} // namespace ZHLN
