#pragma once
#include "Commands.hpp"

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
	std::array<float, 16> lightSpaceMatrix; // Biased (for Sampling in PBR)
	std::array<float, 16> shadowProjView;	// UNBIASED (for Rendering in Shadow Pass)
	float camPos[4];
	float lightDir[4];
	uint32_t lightCount;
	VkDescriptorSet globalSet;
	VkBuffer vbo;
	VkBuffer ibo;
};

struct Light {
	float position[3];
	uint32_t type; // 0=Dir, 1=Point, 2=Spot
	float color[3];
	float intensity;
	float direction[3];
	float range;
	float innerConeCos;
	float outerConeCos;
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
// Direct Recording Functions
// ============================================================================

static void DrawShadows(VkCommandBuffer cmd, const ShadowConfig& c) {
	DrawBatchConfig batchCfg = {c.pipeline,	  c.layout,		  c.scene->vbo,
								c.scene->ibo, VK_NULL_HANDLE, VK_SHADER_STAGE_VERTEX_BIT};

	DrawBatch<ShadowPushConstants>(cmd, batchCfg, [&](auto draw) {
		for (const auto& dc : *c.drawCalls) {
			ShadowPushConstants pc = {.mvp = Multiply(c.scene->shadowProjView, dc.worldMatrix)};
			draw(pc, dc.indexCount, dc.firstIndex);
		}
	});
}

static void DrawPBR(VkCommandBuffer cmd, const PBRMainConfig& c) {
	DrawBatchConfig batchCfg = {
		c.pipeline,			c.layout,
		c.scene->vbo,		c.scene->ibo,
		c.scene->globalSet, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT};

	DrawBatch<PBRPushConstants>(cmd, batchCfg, [&](auto draw) {
		for (const auto& dc : *c.drawCalls) {
			PBRPushConstants pc = {
				.mvp = Multiply(c.scene->viewProj, dc.worldMatrix),
				.lightSpaceMatrix = Multiply(c.scene->lightSpaceMatrix, dc.worldMatrix),
				.worldMatrix = dc.worldMatrix,
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

static void DrawFXAA(VkCommandBuffer cmd, const FXAAConfig& c) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.layout, 0, 1, &c.set, 0,
							nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0); // Fullscreen Triangle
}

} // namespace ZHLN::Vk::Passes
