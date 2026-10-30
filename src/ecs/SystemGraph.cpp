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

struct SystemGraphCore::NodePayload {
    SystemGraphCore::ExecutionContext* ctx     = nullptr;
    uint32_t                       nodeIdx = 0;
};

struct SystemGraphCore::ExecutionContext {
    SystemGraphCore*                  graph   = nullptr;
    void*                             carrier = nullptr;
    TaskSystem::Counter               completionCounter {0};
    std::span<ZHLN::Atomic<uint32_t>> dependencyCounts;
    std::span<NodePayload>            payloads;
};

namespace {

struct SmallIndexList {
    static constexpr size_t             kInlineCapacity = 32;
    std::array<uint32_t, kInlineCapacity> inlineItems {};
    std::vector<uint32_t>               heapItems;
    size_t                              inlineCount = 0;

    void Push(uint32_t idx) {
        if (!heapItems.empty()) {
            heapItems.push_back(idx);
            return;
        }
        if (inlineCount < kInlineCapacity) {
            inlineItems[inlineCount++] = idx;
            return;
        }
        heapItems.reserve(kInlineCapacity * 2);
        heapItems.insert(heapItems.end(), inlineItems.begin(), inlineItems.end());
        heapItems.push_back(idx);
    }

    [[nodiscard]] auto View() const noexcept -> std::span<const uint32_t> {
        if (!heapItems.empty()) {
            return heapItems;
        }
        return std::span<const uint32_t>(inlineItems.data(), inlineCount);
    }
};

} // namespace

void SystemGraphCore::BindCommandRegistry(Registry& registry, EntityCommandBuffer::DestroyFn destroy) noexcept {
    _commandRegistry = &registry;
    _commandDestroy  = destroy;
}

void SystemGraphCore::AddSystem(SystemInfo info) {
    _nodes.push_back({.info = std::move(info), .dependents = {}, .initialDependencyCount = 0});
    AdoptLocal(_nodes.back());
}

// The node's state is the graph's to own: it is allocated from the layout the
// signature produced, and the block keeps the layout so teardown knows what to
// destroy. A node whose system names no Local<T> allocates nothing.
void SystemGraphCore::AdoptLocal(Node& node) {
    node.local.Adopt(node.info.local);
    if (node.info.usesCommandBuffer && _commandRegistry != nullptr) {
        node.ecb = std::make_unique<EntityCommandBuffer>(*_commandRegistry, _commandDestroy);
    }
}

bool SystemGraphCore::AddSystemBefore(SystemInfo info, std::string_view beforeSystem) {
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
    auto inserted = _nodes.insert(anchor, Node {.info = std::move(info), .dependents = {}, .initialDependencyCount = 0});
    AdoptLocal(*inserted);
    return true;
}

void SystemGraphCore::DeclareExternalWrites(const char* label, std::vector<ComponentAccess> accesses) {
    if (label == nullptr || accesses.empty()) {
        return;
    }
    _nodes.push_back(
        Node {
            .info                   = SystemInfo {.update_func = nullptr, .name = label, .access_pattern = std::move(accesses), .enabled = true},
            .dependents             = {},
            .initialDependencyCount = 0,
        }
    );
}

[[nodiscard]] bool SystemGraphCore::HasConflict(const SystemInfo& systemA, const SystemInfo& systemB) noexcept {
    for (const auto& accA: systemA.access_pattern) {
        for (const auto& accB: systemB.access_pattern) {
            if (accA.familyId == accB.familyId || accA.familyId == AllComponents || accB.familyId == AllComponents) {
                if (accA.mode == Access::Write || accB.mode == Access::Write) {
                    return true;
                }
            }
        }
    }
    return false;
}

void SystemGraphCore::Compile() {
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

void SystemGraphCore::DispatchTasks(std::span<const TaskSystem::Task> tasks) {
    if (_taskScheduler != nullptr) {
        _taskScheduler->Dispatch(tasks);
    } else {
        TaskSystem::Dispatch(tasks, nullptr);
    }
}

void SystemGraphCore::WaitForTasks(TaskSystem::Counter& completion) {
    if (_taskScheduler != nullptr) {
        _taskScheduler->Wait(completion);
    } else {
        TaskSystem::Wait(&completion);
    }
}

void SystemGraphCore::Execute(void* carrier) {
    if (_nodes.empty()) {
        return;
    }

    constexpr size_t kStackNodeLimit = 64;
    const size_t     nodeCount       = _nodes.size();

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

    ExecutionContext ctx {.graph = this, .carrier = carrier, .completionCounter = {0}, .dependencyCounts = countsSpan, .payloads = payloadsSpan};

    for (uint32_t i = 0; i < nodeCount; ++i) {
        ctx.dependencyCounts[i].store(_nodes[i].initialDependencyCount, std::memory_order::relaxed);
        ctx.payloads[i] = {.ctx = &ctx, .nodeIdx = i};
    }

    const auto isNodeRunnable = [&](uint32_t idx) noexcept -> bool {
        const Node& node = _nodes[idx];
        return node.info.enabled && node.info.update_func != nullptr;
    };

    // Propagate immediately through disabled or declarative-anchor nodes (such as
    // DeclareExternalWrites) without allocating or scheduling fibers for them.
    SmallIndexList runnable;
    SmallIndexList inertQueue;

    for (const uint32_t entryIdx: _entryNodes) {
        if (isNodeRunnable(entryIdx)) {
            runnable.Push(entryIdx);
        } else {
            inertQueue.Push(entryIdx);
        }
    }

    for (size_t cursor = 0; cursor < inertQueue.View().size(); ++cursor) {
        const uint32_t inertIdx = inertQueue.View()[cursor];
        for (const uint32_t depIdx: _nodes[inertIdx].dependents) {
            if (ctx.dependencyCounts[depIdx].fetch_sub(1, std::memory_order::acq_rel) == 1) {
                if (isNodeRunnable(depIdx)) {
                    runnable.Push(depIdx);
                } else {
                    inertQueue.Push(depIdx);
                }
            }
        }
    }

    const std::span<const uint32_t> readyNodes = runnable.View();
    const size_t                    readyCount = readyNodes.size();

    if (readyCount > 0) {
        ctx.completionCounter.value.store(static_cast<uint32_t>(readyCount), std::memory_order::relaxed);

        if (readyCount > 1) {
            constexpr size_t kStackTaskLimit = 32;
            const size_t     forkCount       = readyCount - 1;

            std::array<TaskSystem::Task, kStackTaskLimit> stackTasks {};
            std::vector<TaskSystem::Task>                 heapTasks;
            std::span<TaskSystem::Task>                   tasksSpan;

            if (forkCount <= kStackTaskLimit) {
                tasksSpan = std::span<TaskSystem::Task>(stackTasks.data(), forkCount);
            } else {
                heapTasks.resize(forkCount);
                tasksSpan = std::span<TaskSystem::Task>(heapTasks.data(), forkCount);
            }

            for (size_t i = 0; i < forkCount; ++i) {
                tasksSpan[i] = {.func = TaskThunk, .arg = &ctx.payloads[readyNodes[i + 1]]};
            }

            DispatchTasks(tasksSpan);
        }

        // Run the first ready branch inline on the calling thread while worker
        // fibers steal the remaining sibling branches.
        DispatchNode(ctx, readyNodes[0]);

        WaitForTasks(ctx.completionCounter);
    }

    for (auto& node: _nodes) {
        if (node.info.enabled && node.ecb != nullptr && !node.ecb->Empty()) {
            node.ecb->Playback();
        }
    }
}

void SystemGraphCore::TaskThunk(void* arg) {
    auto* payload = static_cast<NodePayload*>(arg);
    payload->ctx->graph->DispatchNode(*payload->ctx, payload->nodeIdx);
}

void SystemGraphCore::DispatchNode(ExecutionContext& ctx, uint32_t nodeIdx) {
    const auto isNodeRunnable = [&](uint32_t idx) noexcept -> bool {
        const Node& candidate = _nodes[idx];
        return candidate.info.enabled && candidate.info.update_func != nullptr;
    };

    for (;;) {
        const Node& node = _nodes[nodeIdx];

        if (node.info.enabled && node.info.update_func != nullptr && ctx.carrier != nullptr) {
            // Built here, on this dispatcher's own stack: the carrier is shared by
            // every node, the state is not.
            SystemCall call {
                .carrier   = ctx.carrier,
                .local     = node.local.Pointer(),
                .slots     = node.local.Slots(),
                .slotCount = node.local.SlotCount(),
                .ecb       = node.ecb.get(),
            };
            node.info.update_func(&call);
        }

        if (node.dependents.empty()) {
            ctx.completionCounter.value.fetch_sub(1, std::memory_order::release);
            return;
        }

        SmallIndexList runnable;
        SmallIndexList inertQueue;

        for (const uint32_t depIdx: node.dependents) {
            if (ctx.dependencyCounts[depIdx].fetch_sub(1, std::memory_order::acq_rel) == 1) {
                if (isNodeRunnable(depIdx)) {
                    runnable.Push(depIdx);
                } else {
                    inertQueue.Push(depIdx);
                }
            }
        }

        for (size_t cursor = 0; cursor < inertQueue.View().size(); ++cursor) {
            const uint32_t inertIdx = inertQueue.View()[cursor];
            for (const uint32_t depIdx: _nodes[inertIdx].dependents) {
                if (ctx.dependencyCounts[depIdx].fetch_sub(1, std::memory_order::acq_rel) == 1) {
                    if (isNodeRunnable(depIdx)) {
                        runnable.Push(depIdx);
                    } else {
                        inertQueue.Push(depIdx);
                    }
                }
            }
        }

        const std::span<const uint32_t> unlocked  = runnable.View();
        const size_t                    nextCount = unlocked.size();

        if (nextCount == 0) {
            ctx.completionCounter.value.fetch_sub(1, std::memory_order::release);
            return;
        }

        if (nextCount > 1) {
            constexpr size_t kStackTaskLimit = 32;
            const size_t     forkCount       = nextCount - 1;

            std::array<TaskSystem::Task, kStackTaskLimit> stackTasks {};
            std::vector<TaskSystem::Task>                 heapTasks;
            TaskSystem::Task*                             taskBuffer = nullptr;

            if (forkCount <= kStackTaskLimit) {
                taskBuffer = stackTasks.data();
            } else {
                heapTasks.resize(forkCount);
                taskBuffer = heapTasks.data();
            }

            ctx.completionCounter.value.fetch_add(static_cast<uint32_t>(forkCount), std::memory_order::relaxed);
            for (size_t i = 0; i < forkCount; ++i) {
                taskBuffer[i] = {.func = TaskThunk, .arg = &ctx.payloads[unlocked[i + 1]]};
            }

            DispatchTasks(std::span<const TaskSystem::Task>(taskBuffer, forkCount));
        }

        // Tail-continue into unlocked[0] on the current fiber/thread, transferring
        // this node's completionCounter token without an extra fiber hop.
        nodeIdx = unlocked[0];
    }
}

void SystemGraphCore::SetSystemEnabled(std::string_view name, bool enabled) noexcept {
    for (auto& node: _nodes) {
        if (std::string_view(node.info.name) == name) {
            node.info.enabled = enabled;
            break;
        }
    }
}

bool SystemGraphCore::IsSystemEnabled(std::string_view name) const noexcept {
    for (const auto& node: _nodes) {
        if (std::string_view(node.info.name) == name) {
            return node.info.enabled;
        }
    }
    return false;
}

size_t SystemGraphCore::GetSystemCount() const noexcept {
    return _nodes.size();
}

bool SystemGraphCore::IsEmpty() const noexcept {
    return _nodes.empty();
}

void SystemGraphCore::Clear() noexcept {
    // Clearing the nodes destroys the state each one owned, because the storage
    // lives in the node rather than beside it.
    _nodes.clear();
    _entryNodes.clear();
}

}
