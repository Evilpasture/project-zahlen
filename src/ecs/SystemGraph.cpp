// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SystemGraph.hpp"

#include <threading/TaskSystem.hpp>

namespace ZHLN::ECS {

void SystemGraph::AddSystem(SystemInfo info) {
	_nodes.push_back({.info = std::move(info),
					  .dependents = {},
					  .initialDependencyCount = 0,
					  .currentDependencyCount = {0}});
}

[[nodiscard]] bool SystemGraph::HasConflict(const SystemInfo& systemA,
											const SystemInfo& systemB) const noexcept {
	for (const auto& accA : systemA.access_pattern) {
		for (const auto& accB : systemB.access_pattern) {
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
	_payloads.resize(_nodes.size());
	_entryNodes.clear();

	for (uint32_t i = 0; i < _nodes.size(); ++i) {
		_nodes[i].dependents.clear();
		_nodes[i].initialDependencyCount = 0;
		_payloads[i] = {.graph = this, .nodeIdx = i};
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

	_currentEngine = &engine;
	_currentDt = dt;

	uint32_t enabledCount = 0;
	for (auto& _node : _nodes) {
		_node.currentDependencyCount.store(_node.initialDependencyCount, std::memory_order::relaxed);
		if (_node.info.enabled) {
			enabledCount++;
		}
	}

	if (enabledCount == 0) {
		return;
	}

	_completionCounter.store(enabledCount, std::memory_order::release);

	std::vector<TaskSystem::Task> initialTasks;
	initialTasks.reserve(_entryNodes.size());

	for (uint32_t idx : _entryNodes) {
		initialTasks.push_back({.func = TaskThunk, .arg = &_payloads[idx]});
	}

	if (!initialTasks.empty()) {
		TaskSystem::Dispatch(initialTasks, nullptr);
	}

	auto* counterPtr = reinterpret_cast<TaskSystem::Counter*>(&_completionCounter);
	TaskSystem::Wait(counterPtr);
}

void SystemGraph::TaskThunk(void* arg) {
	auto* payload = static_cast<ExecPayload*>(arg);
	payload->graph->DispatchNode(payload->nodeIdx);
}

void SystemGraph::DispatchNode(uint32_t nodeIdx) {
	Node& node = _nodes[nodeIdx];

	if (node.info.enabled) {
		node.info.update_func(*_currentEngine, _currentDt);
		_completionCounter.fetch_sub(1, std::memory_order::release);
	}

	std::vector<TaskSystem::Task> nextTasks;
	for (uint32_t depIdx : node.dependents) {
		if (_nodes[depIdx].currentDependencyCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			nextTasks.push_back({.func = TaskThunk, .arg = &_payloads[depIdx]});
		}
	}

	if (!nextTasks.empty()) {
		TaskSystem::Dispatch(nextTasks, nullptr);
	}
}

void SystemGraph::SetSystemEnabled(std::string_view name, bool enabled) {
	for (auto& node : _nodes) {
		if (node.info.name == name) {
			node.info.enabled = enabled;
			break;
		}
	}
}

} // namespace ZHLN::ECS
