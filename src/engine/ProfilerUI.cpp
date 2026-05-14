#include "Allocator.hpp" // For MemoryStats
#include "Zahlen/Components.hpp"

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

		float totalTime = 0.0f;
		for (auto const& [name, data] : Profiler::GetMetrics()) {
			totalTime += data.cpuTimeMS;
		}

		ImGui::SeparatorText("Frame Budget (16.6ms)");
		float budgetPercent = totalTime / 16.66f;
		ImVec4 budgetColor = budgetPercent > 0.9f ? ImVec4(1, 0, 0, 1) : ImVec4(0, 1, 0, 1);
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, budgetColor);
		ImGui::ProgressBar(budgetPercent, ImVec2(-1, 25),
						   std::format("%.2f ms", totalTime).c_str());
		ImGui::PopStyleColor();
	}
	ImGui::End();
}
} // namespace ZHLN