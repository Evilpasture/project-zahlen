#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <chrono>
#include <imgui.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ZHLN {

namespace {

struct ProfileDataInternal {
	float cpuTimeMS = 0.0f;
	float rollingAverageMS = 0.0f;
	std::vector<float> history;
};

static std::map<std::string, ProfileDataInternal, std::less<>> s_Metrics;
static ZHLN::Mutex s_ProfilerMutex;

} // namespace

// ============================================================================
// Profiler & ScopedTimer Static Implementations
// ============================================================================

void CPUProfiler::Record(std::string_view name, float timeMS) noexcept {
	std::lock_guard<ZHLN::Mutex> lock(s_ProfilerMutex);

	// 1. Transparent Lookup: Compares string_view against std::string nodes directly in-place
	auto it = s_Metrics.find(name);

	// 2. Only allocate a persistent std::string if the stage doesn't exist yet
	if (it == s_Metrics.end()) {
		it = s_Metrics.emplace(std::string(name), ProfileDataInternal{}).first;
	}

	auto& data = it->second;
	data.cpuTimeMS = timeMS;
	data.rollingAverageMS = data.rollingAverageMS * 0.95f + timeMS * 0.05f;

	data.history.push_back(timeMS);
	if (data.history.size() > 100) {
		data.history.erase(data.history.begin());
	}
}

void CPUProfiler::IterateMetrics(MetricCallback callback, void* userData) noexcept {
	if (callback == nullptr) {
		return;
	}
	std::lock_guard<ZHLN::Mutex> lock(s_ProfilerMutex);
	for (const auto& [name, data] : s_Metrics) {
		callback(name.c_str(), data.cpuTimeMS, data.rollingAverageMS, data.history.data(),
				 data.history.size(), userData);
	}
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

} // namespace ZHLN
