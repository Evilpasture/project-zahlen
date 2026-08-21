// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

struct SystemGraph::NodePayload {
    SystemGraph::ExecutionContext* ctx     = nullptr;
    uint32_t                       nodeIdx = 0;
};

struct SystemGraph::ExecutionContext {
    SystemGraph*                      graph  = nullptr;
    ZHLN::Engine*                     engine = nullptr;
    float                             dt     = 0.0f;
    TaskSystem::Counter               completionCounter {0};
    std::span<ZHLN::Atomic<uint32_t>> dependencyCounts;
    std::span<NodePayload>            payloads;
};

void SystemGraph::AddSystem(SystemInfo info) {
    _nodes.push_back({.info = std::move(info), .dependents = {}, .initialDependencyCount = 0});
}

bool SystemGraph::AddSystemBefore(SystemInfo info, std::string_view beforeSystem) {
    if (info.name == nullptr || beforeSystem.empty()) {
        return false;
    }
    for (const Node& node: _nodes) {
        if (std::string_view(node.info.name) == std::string_view(info.name)) {
            return false;
        }
    }
    const auto anchor = std::ranges::find_if(_nodes, [&](const Node& node) { return std::string_view(node.info.name) == beforeSystem; });
    if (anchor == _nodes.end()) {
        return false;
    }
    _nodes.insert(anchor, Node {.info = std::move(info), .dependents = {}, .initialDependencyCount = 0});
    return true;
}

[[nodiscard]] bool SystemGraph::HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept {
    for (const auto& accA: systemA.access_pattern) {
        for (const auto& accB: systemB.access_pattern) {
            if (accA.familyId == accB.familyId) {
                if (accA.mode == Access::Write || accB.mode == Access::Write) {
                    return true;
                }
            }
        }
    }
    return false;
}

void SystemGraph::Compile() {
    _entryNodes.clear();

    for (auto& node: _nodes) {
        node.dependents.clear();
        node.initialDependencyCount = 0;
    }

    for (uint32_t i = 0; i < _nodes.size(); ++i) {
        for (uint32_t j = i + 1; j < _nodes.size(); ++j) {
            if (HasConflict(_nodes[i].info, _nodes[j].info)) {
                _nodes[i].dependents.push_back(j);
                _nodes[j].initialDependencyCount++;
            }
        }
        if (_nodes[i].initialDependencyCount == 0) {
            _entryNodes.push_back(i);
        }
    }
}

void SystemGraph::Execute(ZHLN::Engine& engine, float dt) {
    if (_nodes.empty()) {
        return;
    }

    constexpr size_t kStackNodeLimit = 64;
    const size_t     nodeCount       = _nodes.size();

    // Stack buffers for zero-allocation execution on graphs with up to 64 systems
    std::array<ZHLN::Atomic<uint32_t>, kStackNodeLimit> stackCounts {};
    std::array<NodePayload, kStackNodeLimit>            stackPayloads {};

    std::vector<ZHLN::Atomic<uint32_t>> heapCounts;
    std::vector<NodePayload>            heapPayloads;

    std::span<ZHLN::Atomic<uint32_t>> countsSpan;
    std::span<NodePayload>            payloadsSpan;

    if (nodeCount <= kStackNodeLimit) {
        countsSpan   = std::span<ZHLN::Atomic<uint32_t>>(stackCounts.data(), nodeCount);
        payloadsSpan = std::span<NodePayload>(stackPayloads.data(), nodeCount);
    } else {
        heapCounts.resize(nodeCount);
        heapPayloads.resize(nodeCount);
        countsSpan   = std::span<ZHLN::Atomic<uint32_t>>(heapCounts.data(), nodeCount);
        payloadsSpan = std::span<NodePayload>(heapPayloads.data(), nodeCount);
    }

    ExecutionContext ctx {.graph = this, .engine = &engine, .dt = dt, .completionCounter = {0}, .dependencyCounts = countsSpan, .payloads = payloadsSpan};

    for (uint32_t i = 0; i < nodeCount; ++i) {
        ctx.dependencyCounts[i].store(_nodes[i].initialDependencyCount, std::memory_order::relaxed);
        ctx.payloads[i] = {.ctx = &ctx, .nodeIdx = i};
    }

    constexpr size_t kStackTaskLimit = 32;
    const size_t     entryCount      = _entryNodes.size();

    std::array<TaskSystem::Task, kStackTaskLimit> stackTasks {};
    std::vector<TaskSystem::Task>                 heapTasks;
    std::span<TaskSystem::Task>                   tasksSpan;

    if (entryCount <= kStackTaskLimit) {
        tasksSpan = std::span<TaskSystem::Task>(stackTasks.data(), entryCount);
    } else {
        heapTasks.resize(entryCount);
        tasksSpan = std::span<TaskSystem::Task>(heapTasks.data(), entryCount);
    }

    for (size_t i = 0; i < entryCount; ++i) {
        ctx.completionCounter.value.fetch_add(1, std::memory_order::relaxed);
        tasksSpan[i] = {.func = TaskThunk, .arg = &ctx.payloads[_entryNodes[i]]};
    }

    if (entryCount > 0) {
        TaskSystem::Dispatch(tasksSpan, nullptr);
    }

    TaskSystem::Wait(&ctx.completionCounter);
}

void SystemGraph::TaskThunk(void* arg) {
    auto* payload = static_cast<NodePayload*>(arg);
    payload->ctx->graph->DispatchNode(*payload->ctx, payload->nodeIdx);
}

void SystemGraph::DispatchNode(ExecutionContext& ctx, uint32_t nodeIdx) {
    const Node& node = _nodes[nodeIdx];

    if (node.info.enabled && node.info.update_func != nullptr && ctx.engine != nullptr) {
        node.info.update_func(*ctx.engine, ctx.dt);
    }

    const size_t depCount = node.dependents.size();
    if (depCount > 0) {
        constexpr size_t kStackTaskLimit = 32;

        std::array<TaskSystem::Task, kStackTaskLimit> stackTasks {};
        std::vector<TaskSystem::Task>                 heapTasks;
        TaskSystem::Task*                             taskBuffer = nullptr;

        if (depCount <= kStackTaskLimit) {
            taskBuffer = stackTasks.data();
        } else {
            heapTasks.resize(depCount);
            taskBuffer = heapTasks.data();
        }

        size_t nextCount = 0;
        for (uint32_t depIdx: node.dependents) {
            if (ctx.dependencyCounts[depIdx].fetch_sub(1, std::memory_order_acq_rel) == 1) {
                ctx.completionCounter.value.fetch_add(1, std::memory_order::relaxed);
                taskBuffer[nextCount++] = {.func = TaskThunk, .arg = &ctx.payloads[depIdx]};
            }
        }

        if (nextCount > 0) {
            TaskSystem::Dispatch(std::span<const TaskSystem::Task>(taskBuffer, nextCount), nullptr);
        }
    }

    // Decrement completion counter ONLY AFTER all child tasks have been safely dispatched
    ctx.completionCounter.value.fetch_sub(1, std::memory_order::release);
}

void SystemGraph::SetSystemEnabled(std::string_view name, bool enabled) noexcept {
    for (auto& node: _nodes) {
        if (std::string_view(node.info.name) == name) {
            node.info.enabled = enabled;
            break;
        }
    }
}

bool SystemGraph::IsSystemEnabled(std::string_view name) const noexcept {
    for (const auto& node: _nodes) {
        if (std::string_view(node.info.name) == name) {
            return node.info.enabled;
        }
    }
    return false;
}

size_t SystemGraph::GetSystemCount() const noexcept {
    return _nodes.size();
}

bool SystemGraph::IsEmpty() const noexcept {
    return _nodes.empty();
}

void SystemGraph::Clear() noexcept {
    _nodes.clear();
    _entryNodes.clear();
}

} // namespace ZHLN::ECS
