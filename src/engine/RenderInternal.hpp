// File: src/engine/RenderInternal.hpp
#pragma once

#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "Postprocessing.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "Vertex.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <memory>
#include <vulkan/vulkan_core.h>

namespace ZHLN {

// --- Layouts and Types ---

using GlobalSceneLayout = Vk::DescriptorLayout<
	Vk::BindlessSampledImageSlot<0, 4096>, // Bindless textures
	Vk::SamplerSlot<1>,					   // Default linear sampler
	Vk::SampledImageSlot<2>,			   // Shadow Map (Depth)
	Vk::SamplerSlot<3>,					   // Shadow Map comparison sampler
	Vk::UniformSlot<4, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT>, // FrameUniforms
																				   // (UBO)
	Vk::StorageBufferSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>,						   // Lights (SSBO)
	Vk::StorageBufferSlot<6, VK_SHADER_STAGE_VERTEX_BIT>, // Instance buffer (SSBO)
	Vk::StorageBufferSlot<7, VK_SHADER_STAGE_VERTEX_BIT>, // Joint matrices (SSBO)

	Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // Pre-filtered Cubemap (Specular IBL)
	Vk::SampledImageSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // 2D BRDF LUT (2D Texture)
	Vk::StorageBufferSlot<10, VK_SHADER_STAGE_VERTEX_BIT>,	 // Morph target deltas (SSBO)
	Vk::SamplerSlot<11, VK_SHADER_STAGE_FRAGMENT_BIT>,		 // Clamping
	Vk::SampledImageSlot<12, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // LTC Matrix
	Vk::SampledImageSlot<13, VK_SHADER_STAGE_FRAGMENT_BIT>>; // LTC Amplitude

using TAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SampledImageSlot<1>,
									   Vk::SampledImageSlot<2>, Vk::SamplerSlot<3>>;

using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
										Vk::SamplerSlot<1>,		 // sampler
										Vk::SampledImageSlot<2>, // texDepth
										Vk::SampledImageSlot<3>, // texNormalRoughness
										Vk::SamplerSlot<4>		 // pointSampler (Nearest)
										>;
using CullingLayout = Vk::DescriptorLayout<Vk::StorageBufferSlot<0>, Vk::StorageBufferSlot<1>>;

struct NativeMesh {
	Vk::Buffer buffer;
	uint32_t vertexCount;
};

struct NativeMaterial {
	Vk::Pipeline pipeline;
	Vk::PipelineLayout layout;
};

static constexpr uint32_t kGpuCullingMaxInstances = 8192;
static constexpr uint32_t kGpuCullingMaxBatches = 256;
static constexpr uint32_t kGpuCullingMaxVisibleInstances =
	kGpuCullingMaxInstances * kGpuCullingMaxBatches;

struct DrawCommand {
	NativeMaterial* material;
	NativeMesh* mesh;
	NativeMesh* indexMesh;
	JPH::Mat44 transform;
	JPH::Mat44 prevTransform;
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t pbrIndex;
	uint32_t emissiveIndex;
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint32_t alphaMode;
	uint32_t jointOffset;
	uint32_t isSkinned;
	float baseColorFactor[4];
	uint32_t morphOffset;
	uint32_t activeMorphCount;
	float morphWeights[4];
	uint32_t indexCount = 0;
};

struct UIDrawCommand {
	NativeMesh* mesh;
	uint32_t fontIndex;
};

struct WorkerCmdContext {
	std::array<Vk::CommandPool, 2> pools;
	std::array<ZHLN::Atomic<uint32_t>, 2> cmdCount{};
};

// --- Impl Struct ---

struct RenderContext::Impl {
	Window& window;
	String64 appName;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;
	Vk::PresentationContext presentation;
	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;

	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> sceneColor;
	Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT> velocityBuffer;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> normalRoughnessBuffer;
	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;

	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<BlitLayout> blitPass;

	Vk::Sampler defaultSampler;
	Vk::Sampler pointSampler;

	uint32_t frame_index = 0;
	bool resized = true;
	bool needsInitialClear = true;
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj{};
	JPH::Mat44 unjittered_view_proj{};
	JPH::Mat44 shadowProjView{};
	FrameUniforms currentUniforms{};

	JPH::Array<std::unique_ptr<NativeMesh>> meshes;
	JPH::Array<std::unique_ptr<NativeMaterial>> materials;
	JPH::Array<DrawCommand> drawQueue;
	JPH::Array<WorkerCmdContext> workerCmds;

	bool depth_ready = false;
	Vk::DescriptorPool uiPool;

	Vk::DescriptorSetLayout bindlessLayout;
	Vk::DescriptorPool bindlessPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> bindlessSets;
	Vk::Sampler globalSampler;

	uint32_t nextTextureIndex = 0;
	JPH::Array<Vk::Image> textureImages;
	JPH::Array<Vk::ImageView> textureViews;

	Vk::DescriptorSetLayout cullingLayout;
	Vk::DescriptorPool cullingPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> cullingSets;

	static constexpr uint32_t SHADOW_RES = 2048;
	Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMap;
	Vk::Sampler shadowSampler;
	Vk::Sampler clampSampler;

	Vk::Image ltcMatImage;
	Vk::ImageView ltcMatView;
	Vk::Image ltcAmpImage;
	Vk::ImageView ltcAmpView;

	ZHLN::DoubleBuffered<Vk::Buffer> frameUniformBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightStorageBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> instanceDataBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> indirectCommandsBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> jointBuffers; // Global Joint Transforms SSBO
	Vk::Pipeline shadowPipeline;
	Vk::PipelineLayout shadowPipelineLayout;
	Vk::Pipeline cullingPipeline;
	Vk::PipelineLayout cullingPipelineLayout;

	JPH::Array<UIDrawCommand> uiDrawQueue;
	Vk::Pipeline uiPipeline;
	Vk::PipelineLayout uiPipelineLayout;

	std::array<JPH::Vec4, 9> shCoeffs{};

	Vk::Image prefilteredImage;
	Vk::ImageView prefilteredView;
	Vk::Image brdfLutImage;
	Vk::ImageView brdfLutView;

	Vk::Buffer morphDeltasBuffer; // Holds all packed morph target deltas
	uint32_t nextMorphDeltaIndex = 0;

	Impl(Window& win) : window(win) {}

	void InitShadowResources();
	void InitCullingResources();
	void CompileShadowPipeline(VkDevice device, const void* shaderData, size_t shaderSize);
	void InitBindless();
	void InitPostProcessing();
	void SetupUI(GLFWwindow* window);

	// Decomposed Rendering Stage Helpers
	void RenderShadowPass(VkCommandBuffer cmd);
	bool RenderMainPassGpuCulling(RenderContext& ctx, VkCommandBuffer cmd);
	void RenderMainPass(RenderContext& ctx, VkCommandBuffer cmd);
	void ApplyTAAPass(VkCommandBuffer cmd, VkExtent2D extent);
	void BlitAndDrawUI(VkCommandBuffer cmd, VkExtent2D extent, uint32_t imageIdx);
	void SubmitFrame();
};

} // namespace ZHLN

namespace ZHLN::Vk {
template <> struct FormatOf<float[3]> {
	static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <> struct FormatOf<::ZHLN::Packed1010102> {
	static constexpr auto value = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
};
template <> struct FormatOf<::ZHLN::PackedHalf2> {
	static constexpr auto value = VK_FORMAT_R16G16_SFLOAT;
};
template <> struct FormatOf<::ZHLN::PackedRGBA8> {
	static constexpr auto value = VK_FORMAT_R8G8B8A8_UNORM;
};
} // namespace ZHLN::Vk

ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color, joints, weights);
