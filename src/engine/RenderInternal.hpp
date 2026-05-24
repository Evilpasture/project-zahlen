// File: src/engine/RenderInternal.hpp
#pragma once

#include "Allocator.hpp"
#include "DescriptorLayout.hpp"
#include "Postprocessing.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "Vertex.hpp"

#include <Zahlen/Types.hpp>
#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <array>
#include <memory>
#include <vector>
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
	Vk::StorageBufferSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>,				   // Lights (SSBO)
	Vk::StorageBufferSlot<6, VK_SHADER_STAGE_VERTEX_BIT>					   // Instance buffer (SSBO)
	>;

using TAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SampledImageSlot<1>,
									   Vk::SampledImageSlot<2>, Vk::SamplerSlot<3>>;

using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SamplerSlot<1>>;
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
	JPH::Mat44 transform;
	JPH::Mat44 prevTransform;
	uint32_t albedoIndex;
	uint32_t normalIndex;
	uint32_t pbrIndex;
	uint32_t emissiveIndex;
	float cullRadius;
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
	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;

	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<BlitLayout> blitPass;

	Vk::Sampler defaultSampler;

	uint32_t frame_index = 0;
	bool resized = true;
	bool needsInitialClear = true;
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj{};
	JPH::Mat44 prev_view_proj{};
	JPH::Mat44 shadowProjView{};

	JPH::Array<std::unique_ptr<NativeMesh>> meshes;
	JPH::Array<std::unique_ptr<NativeMaterial>> materials;
	JPH::Array<DrawCommand> drawQueue;
	JPH::Array<WorkerCmdContext> workerCmds;

	bool depth_ready = false;
	Vk::DescriptorPool uiPool;

	Vk::DescriptorSetLayout bindlessLayout;
	Vk::DescriptorPool bindlessPool;
	VkDescriptorSet bindlessSet = VK_NULL_HANDLE;
	Vk::Sampler globalSampler;

	uint32_t nextTextureIndex = 0;
	JPH::Array<Vk::Image> textureImages;
	JPH::Array<Vk::ImageView> textureViews;

	Vk::DescriptorSetLayout cullingLayout;
	Vk::DescriptorPool cullingPool;
	VkDescriptorSet cullingSet = VK_NULL_HANDLE;

	static constexpr uint32_t SHADOW_RES = 2048;
	Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMap;
	Vk::Sampler shadowSampler;

	Vk::Buffer frameUniformBuffer;
	Vk::Buffer lightStorageBuffer;

	Vk::Buffer instanceDataBuffer;
	Vk::Buffer indirectCommandsBuffer;

	Vk::Pipeline shadowPipeline;
	Vk::PipelineLayout shadowPipelineLayout;
	Vk::Pipeline cullingPipeline;
	Vk::PipelineLayout cullingPipelineLayout;

	JPH::Array<UIDrawCommand> uiDrawQueue;
	Vk::Pipeline uiPipeline;
	Vk::PipelineLayout uiPipelineLayout;

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

ZHLN_REFLECT_VERTEX(::ZHLN::Vertex, position, normal, tangent, uv, color);
