// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ECS.hpp"
#include "threading/TaskSystem.hpp"
#include <detail/Atomic.hpp>
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

    void AddSystem(SystemInfo info);
    void Compile();
    void Execute(ZHLN::Engine& engine, float dt);

    void SetSystemEnabled(std::string_view name, bool enabled);

  private:
    struct Node {
        SystemInfo             info;
        std::vector<uint32_t>  dependents;
        uint32_t               initialDependencyCount = 0;
        ZHLN::Atomic<uint32_t> currentDependencyCount {0};
    };

    struct ExecPayload {
        SystemGraph* graph;
        uint32_t     nodeIdx;
    };

    [[nodiscard]] bool HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) const noexcept;
    static void        TaskThunk(void* arg);
    void               DispatchNode(uint32_t nodeIdx);

    std::vector<Node>        _nodes;
    std::vector<ExecPayload> _payloads;
    std::vector<uint32_t>    _entryNodes;

    ZHLN::Engine*       _currentEngine = nullptr;
    float               _currentDt     = 0.0f;
    TaskSystem::Counter _completionCounter;
};

} // namespace ZHLN::ECS
