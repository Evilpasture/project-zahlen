// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN {
class Engine;
}

namespace ZHLN::ECS {

enum class Access : uint8_t { Read, Write };

struct ComponentAccess {
    uint32_t familyId;
    Access   mode;
};

template <typename T>
constexpr auto Read() noexcept -> ComponentAccess {
    return {ComponentFamily::GetTypeID<T>(), Access::Read};
}

template <typename T>
constexpr auto Write() noexcept -> ComponentAccess {
    return {ComponentFamily::GetTypeID<T>(), Access::Write};
}

using SystemFunc = void (*)(ZHLN::Engine&, float);

struct SystemInfo {
    SystemFunc                   update_func = nullptr;
    const char*                  name        = "UnnamedSystem";
    std::vector<ComponentAccess> access_pattern;
    bool                         enabled = true;
};

class ZHLN_API SystemGraph {
  public:
    SystemGraph()  = default;
    ~SystemGraph() = default;

    SystemGraph(const SystemGraph&)                        = delete;
    auto operator=(const SystemGraph&) -> SystemGraph&     = delete;
    SystemGraph(SystemGraph&&) noexcept                    = default;
    auto operator=(SystemGraph&&) noexcept -> SystemGraph& = default;

    void AddSystem(SystemInfo info);
    /** Inserts an optional subsystem before a named phase; returns false on duplicate/missing anchor. */
    auto AddSystemBefore(SystemInfo info, std::string_view beforeSystem) -> bool;
    void Compile();
    void Execute(ZHLN::Engine& engine, float dt);

    void               SetSystemEnabled(std::string_view name, bool enabled) noexcept;
    [[nodiscard]] auto IsSystemEnabled(std::string_view name) const noexcept -> bool;
    [[nodiscard]] auto GetSystemCount() const noexcept -> size_t;
    [[nodiscard]] auto IsEmpty() const noexcept -> bool;
    void               Clear() noexcept;

    [[nodiscard]] static auto HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept -> bool;

  private:
    struct Node {
        SystemInfo            info;
        std::vector<uint32_t> dependents;
        uint32_t              initialDependencyCount = 0;
    };

    struct ExecutionContext;
    struct NodePayload;

    static void TaskThunk(void* arg);
    void        DispatchNode(ExecutionContext& ctx, uint32_t nodeIdx);

    std::vector<Node>     _nodes;
    std::vector<uint32_t> _entryNodes;
};

} // namespace ZHLN::ECS
