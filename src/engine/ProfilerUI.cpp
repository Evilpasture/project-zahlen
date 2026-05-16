#include "Allocator.hpp" // For MemoryStats
#include "Zahlen/Components.hpp"
#include "engine/RenderState.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <imgui.h>

namespace ZHLN {
void DrawProfiler(Engine& engine) {
	if (ImGui::Begin("Zahlen Profiler")) {

		// 1. CPU TIMINGS (Works via static Profiler class)
		if (ImGui::CollapsingHeader("CPU Timings", ImGuiTreeNodeFlags_DefaultOpen)) {
			for (auto const& [name, data] : Profiler::GetMetrics()) {
				ImGui::Text("%-20s: %.3f ms (Avg: %.3f)", name.c_str(), data.cpuTimeMS,
							data.rollingAverageMS);

				std::string label = "##" + name;
				if (!data.history.empty()) {
					ImGui::PlotLines(label.c_str(), data.history.data(), (int)data.history.size(),
									 0, nullptr, 0.0f, 16.0f, ImVec2(0, 30));
				}
			}
		}

		// 2. PHYSICS STATS (Uses the new bridge method)
		if (ImGui::CollapsingHeader("Physics Stats")) {
			auto& pc = engine.GetPhysicsContext();
			ImGui::BulletText("Active Bodies: %u", pc.GetActiveBodyCount());
			ImGui::BulletText("Total Bodies: %zu", pc.GetWorld().count.load());
		}

		// 3. MEMORY STATS (Works via static MemoryStats class)
		if (ImGui::CollapsingHeader("Memory Usage")) {
			size_t physicsMem = engine.GetPhysicsContext().GetMemoryUsage();
			float mb = physicsMem / (1024.0f * 1024.0f);
			ImGui::Text("Physics Temp Allocator: %.2f MB", mb);

			// Also show the number of entities to see ECS footprint
			ImGui::Text("ECS Entities: %zu",
						engine.GetRegistry().GetEntitiesWith<PhysicsComponent>().size());
		}

		// 4. RENDERER INFO (Uses the new bridge method)
		if (ImGui::CollapsingHeader("Vulkan Info")) {
			auto& rc = engine.GetRenderContext();
			ImGui::Text("GPU: %s", rc.GetGPUName());
			ImGui::Text("API: %s", rc.GetRendererName());

			auto size = engine.GetWindow().GetSize();
			ImGui::Text("Resolution: %ux%u", size.width, size.height);
		}

		// --- NEW: CULLING STATS SECTION ---
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

			// Visual bar showing how much of the scene is being skipped
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.7f, 0.2f, 1.0f));
			ImGui::ProgressBar(culledRatio, ImVec2(-1, 0),
							   std::format("{:.1f}%% Culled", culledRatio * 100.0f).c_str());
			ImGui::PopStyleColor();
		}

		if (ImGui::CollapsingHeader("Anti-Aliasing", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable TAA", &g_TAAState.enabled);
			if (g_TAAState.enabled) {
				ImGui::SliderFloat("TAA Blend", &g_TAAState.feedback, 0.8f, 0.99f);
			}
		}

		float totalTime = 0.0f;
		for (auto const& [name, data] : Profiler::GetMetrics()) {
			totalTime += data.cpuTimeMS;
		}

		// --- FPS AND FRAME BUDGET ---
		ImGui::SeparatorText("Performance Overview");

		// Use ImGui's built-in FPS tracker (rolling average)
		float fps = ImGui::GetIO().Framerate;
		float frameTime = 1000.0f / fps;

		// Display FPS and Wall-Clock Frame Time
		ImGui::Text("FPS: %.1f", fps);
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "(%.2f ms/frame)", frameTime);

		// Budget Bar
		float budgetPercent = totalTime / 16.66f;
		ImVec4 budgetColor = budgetPercent > 0.9f ? ImVec4(1, 0, 0, 1) : ImVec4(0, 1, 0, 1);

		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, budgetColor);
		// We use std::format to show the profiled CPU time vs the 16.6ms target
		ImGui::ProgressBar(budgetPercent, ImVec2(-1, 25),
						   std::format("Profiled: {:.2f} ms / 16.6ms", totalTime).c_str());
		ImGui::PopStyleColor();

		static float fps_history[100] = {};
		static int offset = 0;
		fps_history[offset] = fps;
		offset = (offset + 1) % 100;

		ImGui::PlotHistogram("##FPS", fps_history, 100, offset, nullptr, 0.0f, 165.0f,
							 ImVec2(-1, 40));

		// Optional: Tooltip to explain the difference
		if (ImGui::IsItemHovered()) {
			ImGui::SetTooltip("Profiled time is the sum of instrumented code.\n"
							  "Wall-clock time (%.2fms) includes driver overhead and VSync wait.",
							  frameTime);
		}
	}
	ImGui::End();
}
} // namespace ZHLN