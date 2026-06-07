#include "Allocator.hpp" // For MemoryStats
#include "Zahlen/Components.hpp"
#include "Zahlen/Render.hpp"
#include "ecs/ECS.hpp"
#include "engine/RenderState.hpp"
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

static std::map<std::string, ProfileDataInternal> s_Metrics;
static std::mutex s_ProfilerMutex;

} // namespace

// ============================================================================
// Profiler & ScopedTimer Static Implementations
// ============================================================================

void Profiler::Record(const char* name, float timeMS) noexcept {
	std::lock_guard<std::mutex> lock(s_ProfilerMutex);
	auto& data = s_Metrics[name];
	data.cpuTimeMS = timeMS;
	data.rollingAverageMS = data.rollingAverageMS * 0.95f + timeMS * 0.05f;

	data.history.push_back(timeMS);
	if (data.history.size() > 100) {
		data.history.erase(data.history.begin());
	}
}

void Profiler::IterateMetrics(MetricCallback callback, void* userData) noexcept {
	if (!callback)
		return;
	std::lock_guard<std::mutex> lock(s_ProfilerMutex);
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
	Profiler::Record(name, duration);
}

// ============================================================================
// DrawProfiler Interface
// ============================================================================

void DrawProfiler(Engine& engine) {
	if (ImGui::Begin("Zahlen Profiler")) {

		// 1. CPU TIMINGS
		if (ImGui::CollapsingHeader("CPU Timings", ImGuiTreeNodeFlags_DefaultOpen)) {
			Profiler::IterateMetrics(
				[](const char* name, float cpuTimeMS, float rollingAverageMS, const float* history,
				   size_t historyCount, void*) {
					ImGui::Text("%-20s: %.3f ms (Avg: %.3f)", name, cpuTimeMS, rollingAverageMS);

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
			ImGui::Checkbox("Enable TAA", &g_TAAState.enabled);
			if (g_TAAState.enabled) {
				ImGui::SliderFloat("TAA Blend", &g_TAAState.feedback, 0.8f, 0.99f);
			}
		}

		float totalTime = 0.0f;
		Profiler::IterateMetrics(
			[](const char*, float cpuTimeMS, float, const float*, size_t, void* userData) {
				*static_cast<float*>(userData) += cpuTimeMS;
			},
			&totalTime);

		// --- FPS AND FRAME BUDGET ---
		ImGui::SeparatorText("Performance Overview");

		float fps = ImGui::GetIO().Framerate;
		float frameTime = 1000.0f / fps;

		ImGui::Text("FPS: %.1f", fps);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%.2f ms/frame)", frameTime);

		float budgetPercent = totalTime / 16.66f;
		ImVec4 budgetColor = budgetPercent > 0.9f ? ImVec4(1, 0, 0, 1) : ImVec4(0, 1, 0, 1);

		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, budgetColor);
		ImGui::ProgressBar(budgetPercent, ImVec2(-1, 25),
						   std::format("Profiled: {:.2f} ms / 16.6ms", totalTime).c_str());
		ImGui::PopStyleColor();

		static float fps_history[100] = {};
		static int offset = 0;
		fps_history[offset] = fps;
		offset = (offset + 1) % 100;

		ImGui::PlotHistogram("##FPS", fps_history, 100, offset, nullptr, 0.0f, 165.0f,
							 ImVec2(-1, 40));

		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Profiled time is the sum of instrumented code.\n"
							  "Wall-clock time (%.2fms) includes driver overhead and VSync wait.",
							  frameTime);
		}
	}
	ImGui::End();
}

} // namespace ZHLN
