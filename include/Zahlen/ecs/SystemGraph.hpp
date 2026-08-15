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
constexpr ComponentAccess Read() noexcept {
    return {ComponentFamily::GetTypeID<T>(), Access::Read};
}

template <typename T>
constexpr ComponentAccess Write() noexcept {
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

    SystemGraph(const SystemGraph&)                = delete;
    SystemGraph& operator=(const SystemGraph&)     = delete;
    SystemGraph(SystemGraph&&) noexcept            = default;
    SystemGraph& operator=(SystemGraph&&) noexcept = default;

    void AddSystem(SystemInfo info);
    void Compile();
    void Execute(ZHLN::Engine& engine, float dt);

    void                 SetSystemEnabled(std::string_view name, bool enabled) noexcept;
    [[nodiscard]] bool   IsSystemEnabled(std::string_view name) const noexcept;
    [[nodiscard]] size_t GetSystemCount() const noexcept;
    [[nodiscard]] bool   IsEmpty() const noexcept;
    void                 Clear() noexcept;

    [[nodiscard]] static bool HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept;

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
