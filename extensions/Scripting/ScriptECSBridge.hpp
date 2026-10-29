// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Scripting/ScriptBinder.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

class ZHLN_STATIC_API ScriptECSBridge {
  public:
    explicit ScriptECSBridge(ECS::Registry& reg): m_registry(reg) {
    }

    template <typename Manifest>
    void RegisterComponentManifest() {
        ZHLN::RegisterManifest<Manifest>();
        ZHLN::Reflect::ForEachNestedType<Manifest>([this]<typename Comp>() -> auto { m_registry.RegisterComponent<Comp>(ZHLN::Reflect::TypeName<Comp>()); });
    }

    // --- Entity Component Roots ---
    [[nodiscard]] auto GetProperty(Entity entity, std::string_view compName, std::string_view propName) -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetProperty(Entity entity, std::string_view compName, std::string_view propName, const ScriptVal& val) -> std::expected<void, ErrorCode>;

    // --- Multi-Level Object Drilling ---
    [[nodiscard]] auto GetPropertyOf(const ScriptVal& parentVal, std::string_view propName) const -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetPropertyOf(ScriptVal& parentVal, std::string_view propName, const ScriptVal& val) const -> std::expected<void, ErrorCode>;

    // --- Container Array Operations ---
    [[nodiscard]] auto GetArrayElement(const ScriptVal& arrayVal, size_t index) -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetArrayElement(ScriptVal& arrayVal, size_t index, const ScriptVal& val) -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto GetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index)
        -> std::expected<ScriptVal, ErrorCode>;
    [[nodiscard]] auto SetPropertyElementAt(Entity entity, std::string_view compName, std::string_view propName, size_t index, const ScriptVal& val)
        -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto ResolveBoxedPointer(const BoxedObject& obj) const -> std::expected<void*, ErrorCode>;

  private:
    ECS::Registry& m_registry;
};

} // namespace ZHLN
