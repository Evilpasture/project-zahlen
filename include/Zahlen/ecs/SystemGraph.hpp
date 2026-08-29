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

    /**
     * @brief Declares components written by work that runs *outside* this graph
     *        but completes before Execute().
     *
     * Hazard analysis only ever sees the access patterns declared on nodes. When
     * an imperative frame phase writes a component this graph later reads, the
     * graph sees a reader with no writer and builds no edge at all -- the
     * dependency then exists only in the surrounding call order, invisible to
     * anything inspecting the graph.
     *
     * This inserts an anchor node carrying those writes. It has no update
     * function, and DispatchNode() skips null functions, so it executes nothing
     * and costs one scheduling hop; its only effect is to give hazard analysis a
     * node to hang readers (and any later in-graph writers) off.
     *
     * @param label    Node name. Must point to static storage, like SystemInfo::name.
     * @param accesses Components the external work writes.
     *
     * @note Compile() only builds edges from earlier nodes to later ones, so call
     *       this *before* registering the systems that consume those components.
     */
    void DeclareExternalWrites(const char* label, std::vector<ComponentAccess> accesses);

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
