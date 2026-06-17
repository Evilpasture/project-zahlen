// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderInternal.hpp
#pragma once

#include "Allocator.hpp"
#include "ComputePass.hpp"
#include "DescriptorLayout.hpp"
#include "GpuProfiler.hpp"
#include "Postprocessing.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "StagingContext.hpp"
#include "Vertex.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <detail/MemoryPool.hpp>
#include <memory>
#include <type_traits>

namespace ZHLN::Vk {

struct IBLPayload {
	Image brdfLutImage;
	ImageView brdfLutView;
	Image prefilteredImage;
	ImageView prefilteredView;
	std::array<JPH::Vec4, 9> shCoeffs{};
};

} // namespace ZHLN::Vk

namespace ZHLN {

// ============================================================================
// GenerationalPool Template
// ============================================================================

template <typename T, size_t MaxObjects> class GenerationalPool {
  public:
	GenerationalPool() {
		_freeIndices.reserve(MaxObjects);
		for (size_t i = 0; i < MaxObjects; ++i) {
			_freeIndices.push_back((uint32_t)(MaxObjects - 1 - i));
		}
		_generations.fill(1); // Generations start at 1
	}

	~GenerationalPool() {
		// Automatically sweeps and safely destroys all remaining active allocations on shutdown
		for (size_t i = 0; i < MaxObjects; ++i) {
			if (_pointers[i] != nullptr) {
				_pool.Destroy(_pointers[i]);
			}
		}
	}

	// Non-copyable, non-movable matching engine context lifetime
	GenerationalPool(const GenerationalPool&) = delete;
	auto operator=(const GenerationalPool&) -> GenerationalPool& = delete;

	template <typename... Args> uint64_t Create(Args&&... args) {
		if (_freeIndices.empty()) [[unlikely]] {
			ZHLN::Panic("GenerationalPool exceeded maximum capacity!");
		}
		uint32_t index = _freeIndices.back();
		_freeIndices.pop_back();

		uint32_t gen = _generations[index];
		_pointers[index] = _pool.Create(std::forward<Args>(args)...);

		return (static_cast<uint64_t>(gen) << 32) | index;
	}

	void Destroy(uint64_t handle) {
		auto index = static_cast<uint32_t>(handle & 0xFFFFFFFF);
		auto gen = static_cast<uint32_t>(handle >> 32);

		if (index >= MaxObjects || _generations[index] != gen || _pointers[index] == nullptr) {
			return; // Safely ignore stale or invalid handles
		}

		_pool.Destroy(_pointers[index]);
		_pointers[index] = nullptr;
		_generations[index]++; // Increment generation to invalidate stale handles
		_freeIndices.push_back(index);
	}

	[[nodiscard]] T* Resolve(uint64_t handle) const noexcept {
		auto index = static_cast<uint32_t>(handle & 0xFFFFFFFF);
		auto gen = static_cast<uint32_t>(handle >> 32);

		if (index >= MaxObjects || _generations[index] != gen) {
			return nullptr;
		}
		return _pointers[index];
	}

  private:
	ObjectPool<T, MaxObjects> _pool;
	std::array<T*, MaxObjects> _pointers{};
	std::array<uint32_t, MaxObjects> _generations{};
	JPH::Array<uint32_t> _freeIndices;
};

enum RenderAttachmentSlot : uint8_t {
	ATTACHMENT_SLOT_SCENE_COLOR = 0,
	ATTACHMENT_SLOT_VELOCITY = 1,
	ATTACHMENT_SLOT_ACCUM_0 = 2,
	ATTACHMENT_SLOT_ACCUM_1 = 3,
	ATTACHMENT_SLOT_NORMAL_ROUGHNESS = 4,
	ATTACHMENT_COUNT = 5
};

enum GBufferAttachmentSlot : uint8_t {
	GBUFFER_SLOT_SCENE_COLOR = 0,
	GBUFFER_SLOT_VELOCITY = 1,
	GBUFFER_SLOT_NORMAL_ROUGHNESS = 2,
	GBUFFER_COLOR_COUNT = 3
};

static constexpr uint32_t kGpuCullingSentinel = 4294967295u; // 0xFFFFFFFF [1]
static constexpr Color4 kClearColorNormalRoughness = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};

static constexpr Color4 kClearColorBlack = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f};

static constexpr uint32_t kMainPassColorAttachmentCount = 2;
static constexpr uint32_t kParallelChunkSize = 256;

static constexpr Color4 kClearColorScene = {
	.r = 0.08f, .g = 0.09f, .b = 0.12f, .a = 1.0f}; // G-Buffer background theme
static constexpr Color4 kClearColorVelocity = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f};
static constexpr float kClearDepthValue = 1.0f;

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

	Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>,	// Pre-filtered Cubemap (Specular IBL)
	Vk::SampledImageSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>,	// 2D BRDF LUT (2D Texture)
	Vk::StorageBufferSlot<10, VK_SHADER_STAGE_VERTEX_BIT>,	// Morph target deltas (SSBO)
	Vk::SamplerSlot<11, VK_SHADER_STAGE_FRAGMENT_BIT>,		// Clamping
	Vk::SampledImageSlot<12, VK_SHADER_STAGE_FRAGMENT_BIT>, // LTC Matrix
	Vk::SampledImageSlot<13, VK_SHADER_STAGE_FRAGMENT_BIT>, // LTC Amplitude
	Vk::StorageBufferSlot<14, VK_SHADER_STAGE_VERTEX_BIT>	// Previous Joint matrices (SSBO)
	>;

using TAALayout =
	Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SampledImageSlot<1>, Vk::SampledImageSlot<2>,
						 Vk::SamplerSlot<3>, Vk::UniformSlot<4, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
										Vk::SamplerSlot<1>		 // sampler
										>;

using PostProcessLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
											   Vk::SamplerSlot<1>,		// sampler
											   Vk::SampledImageSlot<2>, // texDepth
											   Vk::SampledImageSlot<3>, // texNormalRoughness
											   Vk::SamplerSlot<4>,		// pointSampler (Nearest)
											   Vk::SampledImageSlot<5>, // texEnvMap (Cubemap)
											   Vk::AccelerationStructureSlot<6> // Hardware TLAS
											   >;

using PostProcessLayoutNoRT = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
												   Vk::SamplerSlot<1>,		// sampler
												   Vk::SampledImageSlot<2>, // texDepth
												   Vk::SampledImageSlot<3>, // texNormalRoughness
												   Vk::SamplerSlot<4>,	   // pointSampler (Nearest)
												   Vk::SampledImageSlot<5> // texEnvMap (Cubemap)
												   >;
using CullingLayout = Vk::DescriptorLayout<Vk::StorageBufferSlot<0>, // g_instances
										   Vk::StorageBufferSlot<1>	 // g_indirectCommands
										   >;

namespace Stages {
struct ShadowPass {
	static constexpr std::string_view name = "[GPU] Shadow Map";
};
struct MainPass {
	static constexpr std::string_view name = "[GPU] G-Buffer/Main";
};
struct AAPass {
	static constexpr std::string_view name = "[GPU] Anti-Aliasing";
};
struct BlitPass {
	static constexpr std::string_view name = "[GPU] Blit/Composite";
};
struct PostProcessPass {
	static constexpr std::string_view name = "[GPU] PostProcess (GI)";
};
} // namespace Stages

using FrameProfiler = Profiler::GpuProfiler<Stages::ShadowPass, Stages::MainPass, Stages::AAPass,
											Stages::PostProcessPass, Stages::BlitPass>;

struct NativeMesh {
	VkDevice device = VK_NULL_HANDLE;
	const Vk::RayTracingContext* rtCtx = nullptr;
	Vk::Buffer buffer;
	uint32_t vertexCount = 0;
	VkDeviceAddress vboAddress = 0;
	VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
	VkDeviceAddress blasAddress = 0;
	Vk::Buffer blasBuffer;

	NativeMesh() = default;
	NativeMesh(Vk::Buffer&& buf, uint32_t count, VkDeviceAddress vboAddr,
			   VkAccelerationStructureKHR b = VK_NULL_HANDLE, VkDeviceAddress addr = 0,
			   Vk::Buffer&& bBuf = {})
		: buffer(std::move(buf)), vertexCount(count), vboAddress(vboAddr), blas(b),
		  blasAddress(addr), blasBuffer(std::move(bBuf)) {}

	~NativeMesh() {
		if (blas != VK_NULL_HANDLE && rtCtx != nullptr) {
			rtCtx->DestroyAS(blas);
		}
	}
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
	std::array<float, 4> baseColorFactor;
	uint32_t morphOffset;
	uint32_t activeMorphCount;
	std::array<float, 4> morphWeights;
	uint32_t indexCount;
	DrawFlags flags;
};

static_assert(std::is_trivially_copyable_v<DrawCommand> &&
			  std::is_trivially_constructible_v<DrawCommand>);

struct UIDrawCommand {
	NativeMesh* mesh;
	uint32_t fontIndex;
};

struct UIObjectConstants {
	JPH::Mat44 orthoMatrix;
	uint64_t vboAddress;
	uint32_t albedoIdx;
	uint32_t padding;
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
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> postProcessTarget;
	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;

	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<BlitLayout> fxaaPass;
	Vk::PostProcessPass<BlitLayout> smaaEdgePass;
	Vk::PostProcessPass<BlitLayout> smaaWeightPass;
	Vk::PostProcessPass<BlitLayout> smaaBlendPass;

	Vk::PostProcessPass<PostProcessLayout> postProcessPass;
	Vk::PostProcessPass<PostProcessLayoutNoRT> postProcessPassNoRT;
	Vk::PostProcessPass<BlitLayout> blitPass;

	Vk::RenderTarget<VK_FORMAT_R8G8_UNORM> smaaEdgeTarget;
	Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> smaaWeightTarget;

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

	// Generational pools (Fully self-contained; no manual trackers needed)
	GenerationalPool<NativeMesh, 1024> meshPool;
	GenerationalPool<NativeMaterial, 512> materialPool;

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

	Vk::IBLPayload iblPayload;
	std::unique_ptr<Vk::StagingContext> stagingContext;

	Vk::RayTracingContext rtCtx;
	DoubleBuffered<VkAccelerationStructureKHR> tlas;
	DoubleBuffered<Vk::Buffer> tlasBuffer;
	DoubleBuffered<Vk::Buffer> tlasScratchBuffer;
	std::array<std::vector<Vk::Buffer>, 2> tlasCleanupBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> frameUniformBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightStorageBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> instanceDataBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> indirectCommandsBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> jointBuffers; // Global Joint Transforms SSBO
	Vk::Pipeline shadowPipeline;
	Vk::PipelineLayout shadowPipelineLayout;
	Vk::ComputePass cullingPass;

	JPH::Array<UIDrawCommand> uiDrawQueue;
	Vk::Pipeline uiPipeline;
	Vk::PipelineLayout uiPipelineLayout;

	GISettings giSettings{};

	Vk::Buffer morphDeltasBuffer; // Holds all packed morph target deltas
	uint32_t nextMorphDeltaIndex = 0;

	ZHLN::DoubleBuffered<BufferHandle> debugMeshHandles;

	AAState aaState{};

	FrameProfiler gpuProfiler;

	Impl(Window& win) : window(win) {}

	void InitShadowResources();
	void InitCullingResources();
	void CompileShadowPipeline(VkDevice device, const void* shaderData, size_t shaderSize);
	void InitBindless();
	void InitPostProcessing();
	void SetupUI(GLFWwindow* window);

	void SortDrawQueue();
	ZHLN_FrameResult SubmitFrame();

	~Impl() {
		if (ctx.Device() != VK_NULL_HANDLE) {
			for (uint32_t i = 0; i < 2; ++i) {
				if (tlas[i] != VK_NULL_HANDLE) {
					rtCtx.DestroyAS(tlas[i]);
				}
			}
		}
	}
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
