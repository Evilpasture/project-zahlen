// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/ProfilerUI.cpp

#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <detail/Atomic.hpp>
#include <detail/SkipList.hpp>
#include <imgui.h>

namespace ZHLN {

namespace {

template <size_t N = 100> struct ProfileDataInternal {
	// Using ZHLN::Atomic preserves trivial copy/construction for the SkipList
	std::array<ZHLN::Atomic<float>, N> history;
	ZHLN::Atomic<size_t> writeIndex;
	ZHLN::Atomic<float> latestValue;
	ZHLN::Atomic<float> rollingAverageMS;

	void Push(float value) noexcept {
		size_t idx = writeIndex.fetch_add(1, std::memory_order_relaxed) % N;
		history[idx].store(value, std::memory_order_relaxed);
		latestValue.store(value, std::memory_order_relaxed);

		// Lock-free rolling average update
		float avg = rollingAverageMS.load(std::memory_order_relaxed);
		float nextAvg = avg * 0.95f + value * 0.05f;
		rollingAverageMS.store(nextAvg, std::memory_order_relaxed);
	}
};

// Safety assert to ensure compilers never silently break the POD contract
static_assert(std::is_trivially_copyable_v<ProfileDataInternal<100>> &&
				  std::is_trivially_default_constructible_v<ProfileDataInternal<100>>,
			  "ProfileDataInternal must remain a POD for SkipList compatibility");

// Stored completely by-value with zero runtime allocations
static SkipList<std::string, ProfileDataInternal<100>, std::less<>> s_Metrics;

} // namespace

// ============================================================================
// Profiler & ScopedTimer Implementations
// ============================================================================

void CPUProfiler::Record(std::string_view name, float timeMS) noexcept {
	std::string key(name);
	const auto* dataPtr = s_Metrics.Find(key);

	if (dataPtr == nullptr) [[unlikely]] {
		s_Metrics.Insert(key, ProfileDataInternal<100>{});
		dataPtr = s_Metrics.Find(key);
	}

	// Safely cast away constness of the stable payload node
	auto* data = const_cast<ProfileDataInternal<100>*>(dataPtr);
	data->Push(timeMS);
}

void CPUProfiler::IterateMetrics(MetricCallback callback, void* userData) noexcept {
	if (callback == nullptr) {
		return;
	}

	s_Metrics.Iterate([&](const std::string& name, const ProfileDataInternal<100>& data) {
		float cpuTime = data.latestValue.load(std::memory_order_relaxed);
		float rollingAvg = data.rollingAverageMS.load(std::memory_order_relaxed);

		size_t count = data.writeIndex.load(std::memory_order_relaxed);
		size_t limit = std::min(count, size_t(100));

		// Reconstruct chronological history on the stack with zero allocations
		std::array<float, 100> flatHistory{};
		if (count < 100) {
			for (size_t j = 0; j < count; ++j) {
				flatHistory[j] = data.history[j].load(std::memory_order_relaxed);
			}
		} else {
			size_t startIdx = count % 100;
			for (size_t j = 0; j < 100; ++j) {
				flatHistory[j] = data.history[(startIdx + j) % 100].load(std::memory_order_relaxed);
			}
		}

		callback(name.c_str(), cpuTime, rollingAvg, flatHistory.data(), limit, userData);
	});
}

ScopedTimer::ScopedTimer(const char* n) noexcept : name(n) {
	auto now = std::chrono::high_resolution_clock::now();
	start = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}

ScopedTimer::~ScopedTimer() noexcept {
	auto now = std::chrono::high_resolution_clock::now();
	uint64_t end =
		std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
	float duration = static_cast<float>(end - start) / 1000.0f; // Microseconds to milliseconds
	CPUProfiler::Record(name, duration);
}

// ============================================================================
// DrawProfiler Interface
// ============================================================================

void DrawProfiler(Engine& engine, TAAState& taaState) {
	if (ImGui::Begin("Zahlen Profiler")) {

		// 1. TIMINGS (Rendered together but clearly separated by name)
		if (ImGui::CollapsingHeader("Performance Timings", ImGuiTreeNodeFlags_DefaultOpen)) {
			CPUProfiler::IterateMetrics(
				[](const char* name, float cpuTimeMS, float rollingAverageMS, const float* history,
				   size_t historyCount, void*) {
					// Color GPU lines slightly differently for instant readability
					bool isGpu = std::string_view(name).starts_with("[GPU]");
					if (isGpu) {
						ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f),
										   "%-25s: %.3f ms (Avg: %.3f)", name, cpuTimeMS,
										   rollingAverageMS);
					} else {
						ImGui::Text("%-25s: %.3f ms (Avg: %.3f)", name, cpuTimeMS,
									rollingAverageMS);
					}

					std::string label = "##" + std::string(name);
					if (historyCount > 0) {
						ImGui::PlotLines(label.c_str(), history, (int)historyCount, 0, nullptr,
										 0.0f, 16.0f, ImVec2(0, 30));
					}
				},
				nullptr);
		}

		// 2. PHYSICS STATS
		if (ImGui::CollapsingHeader("Physics Stats")) {
			auto& pc = engine.GetPhysicsContext();
			ImGui::BulletText("Active Bodies: %u", pc.GetActiveBodyCount());
			ImGui::BulletText("Total Bodies: %zu", pc.GetWorld().count.load());
		}

		// 3. MEMORY STATS
		if (ImGui::CollapsingHeader("Memory Usage")) {
			size_t physicsMem = engine.GetPhysicsContext().GetMemoryUsage();
			float mb = physicsMem / (1024.0f * 1024.0f);
			ImGui::Text("Physics Temp Allocator: %.2f MB", mb);

			ImGui::Text("ECS Entities: %zu",
						engine.GetRegistry().GetEntitiesWith<PhysicsComponent>().size());
		}

		// 4. RENDERER INFO
		if (ImGui::CollapsingHeader("Vulkan Info")) {
			auto& rc = engine.GetRenderContext();
			ImGui::Text("GPU: %s", rc.GetGPUName());
			ImGui::Text("API: %s", rc.GetRendererName());

			auto size = engine.GetWindow().GetSize();
			ImGui::Text("Resolution: %ux%u", size.width, size.height);
		}

		// 5. FRUSTUM CULLING
		if (ImGui::CollapsingHeader("Frustum Culling", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Culling", &CullingStats::EnableCulling);
			ImGui::Checkbox("Freeze Frustum", &CullingStats::FreezeFrustum);

			ImGui::Text("Total Objects:    %u", CullingStats::TotalObjects);
			ImGui::Text("Objects Rendered: %u",
						CullingStats::TotalObjects - CullingStats::CulledObjects);
			ImGui::Text("Objects Culled:   %u", CullingStats::CulledObjects);

			float culledRatio =
				(CullingStats::TotalObjects > 0)
					? (float)CullingStats::CulledObjects / (float)CullingStats::TotalObjects
					: 0.0f;

			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
			ImGui::ProgressBar(culledRatio, ImVec2(-1, 0),
							   std::format("{:.1f}%% Culled", culledRatio * 100.0f).c_str());
			ImGui::PopStyleColor();
		}

		// 6. ANTI-ALIASING
		if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable TAA", &taaState.enabled);
			if (taaState.enabled) {
				ImGui::SliderFloat("TAA Blend", &taaState.feedback, 0.8f, 0.99f);
			}
		}

		// Separate CPU and GPU totals so parallel execution is measured correctly
		struct BudgetPayload {
			float cpuTotal = 0.0f;
			float gpuTotal = 0.0f;
		} totals;

		CPUProfiler::IterateMetrics(
			[](const char* name, float cpuTimeMS, float, const float*, size_t, void* userData) {
				auto* b = static_cast<BudgetPayload*>(userData);
				if (std::string_view(name).starts_with("[GPU]")) {
					b->gpuTotal += cpuTimeMS;
				} else {
					b->cpuTotal += cpuTimeMS;
				}
			},
			&totals);

		// --- FPS AND FRAME BUDGET ---
		ImGui::SeparatorText("Performance Overview");

		float fps = ImGui::GetIO().Framerate;
		float frameTime = 1000.0f / fps;

		ImGui::Text("FPS: %.1f", fps);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%.2f ms/frame)", frameTime);

		// CPU Budget Progress Bar
		float cpuPercent = totals.cpuTotal / 16.66f;
		ImVec4 cpuColor = cpuPercent > 0.9f ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 1, 0.3f, 1);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cpuColor);
		ImGui::ProgressBar(
			cpuPercent, ImVec2(-1, 20),
			std::format("CPU Profiled: {:.2f} ms / 16.6ms", totals.cpuTotal).c_str());
		ImGui::PopStyleColor();

		// GPU Budget Progress Bar
		float gpuPercent = totals.gpuTotal / 16.66f;
		ImVec4 gpuColor =
			gpuPercent > 0.9f ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 0.8f, 1.0f, 1);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, gpuColor);
		ImGui::ProgressBar(
			gpuPercent, ImVec2(-1, 20),
			std::format("GPU Profiled: {:.2f} ms / 16.6ms", totals.gpuTotal).c_str());
		ImGui::PopStyleColor();

		static float fps_history[100] = {};
		static int offset = 0;
		fps_history[offset] = fps;
		offset = (offset + 1) % 100;

		ImGui::PlotHistogram("##FPS", fps_history, 100, offset, nullptr, 0.0f, 250.0f,
							 ImVec2(-1, 40));

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Wall-clock time (%.2fms) includes driver overhead and VSync wait.",
							  frameTime);
		}
	}
	ImGui::End();
}

void DrawECSProfiler() {
	if (ImGui::Begin("Zahlen ECS Profiler")) {
		ImGui::SeparatorText("ECS Systems CPU Execution");

		CPUProfiler::IterateMetrics(
			[](const char* name, float cpuTimeMS, float rollingAverageMS, const float* history,
			   size_t historyCount, void*) {
				std::string_view metricName(name);
				if (metricName.starts_with("ECS System:")) {
					// Strip "ECS System: " prefix for cleaner UI layout
					metricName.remove_prefix(12);

					ImGui::Text("%-30.*s: %.3f ms (Avg: %.3f)", (int)metricName.size(),
								metricName.data(), cpuTimeMS, rollingAverageMS);

					std::string label = "##ECS_" + std::string(metricName);
					if (historyCount > 0) {
						ImGui::PlotLines(label.c_str(), history, (int)historyCount, 0, nullptr,
										 0.0f, 4.0f, ImVec2(-1, 30));
					}
				}
			},
			nullptr);
	}
	ImGui::End();
}

} // namespace ZHLN
