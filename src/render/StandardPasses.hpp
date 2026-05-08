#pragma once
#include "Commands.hpp"
#include "RenderGraph.hpp"

#include <array>
#include <vector>

namespace ZHLN::Vk::Passes {

// --- Data Structures ---

struct PBRDrawCall {
	std::array<float, 16> worldMatrix;
	uint32_t albedoIdx;
	uint32_t normalIdx;
	uint32_t pbrIdx;
	uint32_t lightmapIdx;
	uint32_t emissiveIdx;
	uint32_t indexCount;
	uint32_t firstIndex;
};

struct PBRSceneContext {
	std::array<float, 16> viewProj;
	std::array<float, 16> lightSpaceMatrix;
	float camPos[4];
	float lightDir[4];
	uint32_t lightCount;
	VkDescriptorSet globalSet;
	VkBuffer vbo;
	VkBuffer ibo;
};

// Internal Push Constant structures matching the HLSL shaders perfectly
struct PBRPushConstants {
	std::array<float, 16> mvp;
	std::array<float, 16> lightSpaceMatrix;
	std::array<float, 16> worldMatrix;
	float camPos[4];
	float lightDir[4];
	uint32_t albedoIdx;
	uint32_t normalIdx;
	uint32_t pbrIdx;
	uint32_t lightmapIdx;
	uint32_t emissiveIdx;
	uint32_t lightCount;
	uint32_t _pad[1];
};

struct ShadowPushConstants {
	std::array<float, 16> mvp;
};

// --- Configurations ---

struct ShadowConfig {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	const std::vector<PBRDrawCall>* drawCalls;
	const PBRSceneContext* scene;
};

struct PBRMainConfig {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	const std::vector<PBRDrawCall>* drawCalls;
	const PBRSceneContext* scene;
};

struct FXAAConfig {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	VkDescriptorSet set;
};

// --- Backend Math Helper ---
// Basic 4x4 multiplication
inline constexpr std::array<float, 16> Multiply(const std::array<float, 16>& a,
												const std::array<float, 16>& b) {
	std::array<float, 16> res{};
	for (int c = 0; c < 4; ++c) {
		for (int r = 0; r < 4; ++r) {
			float sum = 0.0f;
			for (int k = 0; k < 4; ++k)
				sum += a[k * 4 + r] * b[c * 4 + k];
			res[c * 4 + r] = sum;
		}
	}
	return res;
}

// ============================================================================
// Internal Recording Functions
// These must match the PassRecordFn signature: void(*)(VkCommandBuffer, const void*)
// ============================================================================

static void RecordShadows(VkCommandBuffer cmd, const void* pUserData) {
	const auto& c = *static_cast<const ShadowConfig*>(pUserData);

	DrawBatchConfig batchCfg = {c.pipeline,	  c.layout,		  c.scene->vbo,
								c.scene->ibo, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT};

	DrawBatch<ShadowPushConstants>(cmd, batchCfg, [&](auto draw) {
		for (const auto& dc : *c.drawCalls) {
			ShadowPushConstants pc = {.mvp = Multiply(c.scene->lightSpaceMatrix, dc.worldMatrix)};
			draw(pc, dc.indexCount, dc.firstIndex);
		}
	});
}

static void RecordPBR(VkCommandBuffer cmd, const void* pUserData) {
	const auto& c = *static_cast<const PBRMainConfig*>(pUserData);

	DrawBatchConfig batchCfg = {
		c.pipeline,			c.layout,
		c.scene->vbo,		c.scene->ibo,
		c.scene->globalSet, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};

	DrawBatch<PBRPushConstants>(cmd, batchCfg, [&](auto draw) {
		for (const auto& dc : *c.drawCalls) {
			PBRPushConstants pc = {
				.mvp = Multiply(c.scene->viewProj, dc.worldMatrix),
				.lightSpaceMatrix = Multiply(c.scene->lightSpaceMatrix, dc.worldMatrix),
				.camPos = {c.scene->camPos[0], c.scene->camPos[1], c.scene->camPos[2], 1.0f},
				.lightDir = {c.scene->lightDir[0], c.scene->lightDir[1], c.scene->lightDir[2],
							 0.0f},
				.albedoIdx = dc.albedoIdx,
				.normalIdx = dc.normalIdx,
				.pbrIdx = dc.pbrIdx,
				.lightmapIdx = dc.lightmapIdx,
				.emissiveIdx = dc.emissiveIdx,
				.lightCount = c.scene->lightCount};
			draw(pc, dc.indexCount, dc.firstIndex);
		}
	});
}

static void RecordFXAA(VkCommandBuffer cmd, const void* pUserData) {
	const auto& c = *static_cast<const FXAAConfig*>(pUserData);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1, &c.set, 0,
							nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0); // Fullscreen Triangle
}

// ============================================================================
// Standard Pass Builders (Refactored to use named functions)
// ============================================================================

inline void AddShadowPass(RenderGraph& graph, GraphImage& shadowMap, const ShadowConfig& cfg) {
	graph.AddPass("Shadow Map Pass").WriteDepth(shadowMap).Record(RecordShadows, &cfg);
}

inline void AddPBRMainPass(RenderGraph& graph, GraphImage& color, GraphImage& depth,
						   GraphImage& shadowMap, const PBRMainConfig& cfg) {
	graph.AddPass("PBR Main Pass")
		.Read(shadowMap)
		.WriteColor(color)
		.WriteDepth(depth)
		.Record(RecordPBR, &cfg);
}

inline void AddFXAAPass(RenderGraph& graph, GraphImage& input, GraphImage& output,
						const FXAAConfig& cfg) {
	graph.AddPass("FXAA Anti-Aliasing").Read(input).WriteColor(output).Record(RecordFXAA, &cfg);
}

} // namespace ZHLN::Vk::Passes

namespace ZHLN::Vk::Nodes {

/**
 * @brief Shadow Map Node
 * Templated by format to allow the compiler to optimize for D16, D32, etc.
 */
template <VkFormat Format> struct ShadowMap {
	struct Config {
		GraphImage& shadowMap;
		const Passes::ShadowConfig& data;
	} config;

	static void Execute(RenderGraph& g, const Config& c) {
		g.AddPass("Shadow Pass")
			.WriteDepth(c.shadowMap)
			.Record(Passes::RecordShadows, &c.data); // Reference standard record fn
	}
};

/**
 * @brief Main Forward PBR Node
 */
template <VkFormat ColorFmt, VkFormat DepthFmt> struct ForwardPBR {
	struct Config {
		GraphImage& color;
		GraphImage& depth;
		GraphImage& shadowMap;
		const Passes::PBRMainConfig& data;
	} config;

	static void Execute(RenderGraph& g, const Config& c) {
		g.AddPass("PBR Main Pass")
			.Read(c.shadowMap)
			.WriteColor(c.color)
			.WriteDepth(c.depth)
			.Record(Passes::RecordPBR, &c.data);
	}
};

/**
 * @brief FXAA Post-Process Node
 */
template <VkFormat OutputFmt> struct FXAA {
	struct Config {
		GraphImage& input;
		GraphImage& output;
		const Passes::FXAAConfig& data;
	} config;

	static void Execute(RenderGraph& g, const Config& c) {
		g.AddPass("FXAA Pass")
			.Read(c.input)
			.WriteColor(c.output)
			.Record(Passes::RecordFXAA, &c.data);
	}
};

} // namespace ZHLN::Vk::Nodes