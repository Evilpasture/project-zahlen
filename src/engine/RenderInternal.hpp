// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderInternal.hpp
#pragma once
#include "Allocator.hpp"
#include "ComputePass.hpp"
#include "DescriptorLayout.hpp"
#include "GpuProfiler.hpp"
#include "Postprocessing.hpp"
#include "PresentationContext.hpp"
#include "RenderCore.hpp"
#include "RenderTarget.hpp"
#include "Resources.hpp"
#include "StagingContext.hpp"
#include "Vertex.hpp"
#include "detail/Array.hpp"
#include "engine/FileWatcher.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <detail/MemoryPool.hpp>
#include <fstream>
#include <memory>
#include <type_traits>
#include <utility>

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

template <uint32_t B, VkShaderStageFlags S = VK_SHADER_STAGE_FRAGMENT_BIT>
using EngineAS = std::conditional_t<isMac,
									Vk::BindingSlot<B, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, S, 1,
													VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT>,
									Vk::AccelerationStructureSlot<B, S>>;

// ============================================================================
// GenerationalPool Template
// ============================================================================

template <typename T, size_t MaxObjects, typename HandleType = uint64_t> class GenerationalPool {
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

	template <typename... Args> HandleType Create(Args&&... args) {
		if (_freeIndices.empty()) [[unlikely]] {
			ZHLN::Log("ERROR: GenerationalPool has exceeded its maximum capacity of {}! Returning "
					  "invalid handle.",
					  MaxObjects);
			return static_cast<HandleType>(0);
		}
		uint32_t index = _freeIndices.back();
		_freeIndices.pop_back();

		uint32_t gen = _generations[index];
		_pointers[index] = _pool.Create(std::forward<Args>(args)...);

		uint64_t packed = (static_cast<uint64_t>(gen) << 32) | index;
		return static_cast<HandleType>(packed);
	}

	void Destroy(HandleType handle) {
		auto rawHandle = static_cast<uint64_t>(handle);
		auto index = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
		auto gen = static_cast<uint32_t>(rawHandle >> 32);

		if (index >= MaxObjects || _generations[index] != gen || _pointers[index] == nullptr) {
			return; // Safely ignore stale or invalid handles
		}

		_pool.Destroy(_pointers[index]);
		_pointers[index] = nullptr;
		_generations[index]++; // Increment generation to invalidate stale handles
		_freeIndices.push_back(index);
	}

	[[nodiscard]] T* Resolve(HandleType handle) const noexcept {
		auto rawHandle = static_cast<uint64_t>(handle);
		auto index = static_cast<uint32_t>(rawHandle & 0xFFFFFFFF);
		auto gen = static_cast<uint32_t>(rawHandle >> 32);

		if (index >= MaxObjects || _generations[index] != gen) {
			return nullptr;
		}
		return _pointers[index];
	}

  private:
	ObjectPool<T, MaxObjects> _pool;
	std::array<T*, MaxObjects> _pointers{};
	std::array<uint32_t, MaxObjects> _generations{};
	ZHLN::Array<uint32_t> _freeIndices;
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

static constexpr uint32_t kGpuCullingSentinel = 0xFFFFFFFF;
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
	Vk::StorageBufferSlot<5, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT>, // Lights
																						 // (SSBO)
	Vk::StorageBufferSlot<6, VK_SHADER_STAGE_VERTEX_BIT>, // Instance buffer (SSBO)
	Vk::StorageBufferSlot<7, VK_SHADER_STAGE_VERTEX_BIT>, // Joint matrices (SSBO)

	Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>,	// Pre-filtered Cubemap (Specular IBL)
	Vk::SampledImageSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>,	// 2D BRDF LUT (2D Texture)
	Vk::StorageBufferSlot<10, VK_SHADER_STAGE_VERTEX_BIT>,	// Morph target deltas (SSBO)
	Vk::SamplerSlot<11, VK_SHADER_STAGE_FRAGMENT_BIT>,		// Clamping
	Vk::SampledImageSlot<12, VK_SHADER_STAGE_FRAGMENT_BIT>, // LTC Matrix
	Vk::SampledImageSlot<13, VK_SHADER_STAGE_FRAGMENT_BIT>, // LTC Amplitude
	Vk::StorageBufferSlot<14, VK_SHADER_STAGE_VERTEX_BIT>,	// Previous Joint matrices (SSBO)
	EngineAS<15, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using TAALayout =
	Vk::DescriptorLayout<Vk::SampledImageSlot<0>, Vk::SampledImageSlot<1>, Vk::SampledImageSlot<2>,
						 Vk::SamplerSlot<3>, Vk::UniformSlot<4, VK_SHADER_STAGE_FRAGMENT_BIT>>;

using FXAALayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
										Vk::SamplerSlot<1>		 // sampler
										>;

using BlitLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texCurrent (Color)
										Vk::SamplerSlot<1>,		 // sampler
										Vk::SampledImageSlot<2>	 // texBloom (Bloom)
										>;

// Layouts for Threshold & Blur
using BloomThresholdLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texInput
												  Vk::SamplerSlot<1>	   // sampler
												  >;

using BlurLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texInput
										Vk::SamplerSlot<1>		 // sampler
										>;

using CullingLayout = Vk::DescriptorLayout<Vk::StorageBufferSlot<0>, // g_instances
										   Vk::StorageBufferSlot<1>	 // g_indirectCommands
										   >;

// Pass 1: Edge Detection Layout (Reads the main scene color)
using SMAAEdgeLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texInput (Color)
											Vk::SamplerSlot<1>,		 // linearSampler
											Vk::SamplerSlot<2>		 // pointSampler
											>;

// Pass 2: Blending Weight Layout (Reads edges, Area LUT, and Search LUT)
using SMAAWeightLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texEdges
											  Vk::SampledImageSlot<1>, // texArea (LUT)
											  Vk::SampledImageSlot<2>, // texSearch (LUT)
											  Vk::SamplerSlot<3>,	   // linearSampler
											  Vk::SamplerSlot<4> // pointSampler (Nearest / Point)
											  >;

// Pass 3: Neighborhood Blending Layout (Blends original color with calculated weights)
using SMAABlendLayout = Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texInput (Color)
											 Vk::SampledImageSlot<1>, // texWeights
											 Vk::SamplerSlot<2>,	  // linearSampler
											 Vk::SamplerSlot<3>		  // pointSampler
											 >;

using ActiveGBuffer =
	Vk::GBufferLayout<Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32>, // Index 0: sceneColor
					  Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT>, // Index 1: velocityBuffer
					  Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> // Index 2: normalRoughnessBuffer
					  >;

using ClusterCullingLayout =
	Vk::DescriptorLayout<Vk::StorageBufferSlot<0>, // ClusterBoundsBuffer (R/W)
						 Vk::StorageBufferSlot<1>, // ClusterGridBuffer (W)
						 Vk::StorageBufferSlot<2>, // LightIndexListBuffer (W)
						 Vk::StorageBufferSlot<3>, // GlobalCounterBuffer (Atomic)
						 Vk::UniformSlot<4, VK_SHADER_STAGE_COMPUTE_BIT>,	   // Frame UBO
						 Vk::StorageBufferSlot<5, VK_SHADER_STAGE_COMPUTE_BIT> // Lights SSBO
						 >;

using AmbientLayout =
	Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texAlbedo
						 Vk::SamplerSlot<1>,	  // sampler
						 Vk::SampledImageSlot<2>, // texDepth
						 Vk::SampledImageSlot<3>, // texNormalRoughness
						 Vk::SamplerSlot<4>,	  // pointSampler
						 Vk::SampledImageSlot<5>, // texEnvMap
						 Vk::SampledImageSlot<6, VK_SHADER_STAGE_FRAGMENT_BIT>, // brdfLUT
						 Vk::SamplerSlot<7, VK_SHADER_STAGE_FRAGMENT_BIT>,		// clampSampler
						 Vk::UniformSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>		// frame
						 >;

using LightingLayout = Vk::DescriptorLayout<
	Vk::SampledImageSlot<0>,								 // texAlbedo
	Vk::SamplerSlot<1>,										 // sampler
	Vk::SampledImageSlot<2>,								 // texDepth
	Vk::SampledImageSlot<3>,								 // texNormalRoughness
	Vk::StorageBufferSlot<4, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // lights
	Vk::UniformSlot<5, VK_SHADER_STAGE_FRAGMENT_BIT>,		 // frame
	Vk::SampledImageSlot<6, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // shadowMap
	Vk::SamplerSlot<7, VK_SHADER_STAGE_FRAGMENT_BIT>,		 // shadowSampler
	Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // ltc_mat
	Vk::SampledImageSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // ltc_amp
	Vk::SamplerSlot<10, VK_SHADER_STAGE_FRAGMENT_BIT>,		 // clampSampler
	Vk::StorageBufferSlot<11, VK_SHADER_STAGE_FRAGMENT_BIT>, // clusterGrid
	Vk::StorageBufferSlot<12, VK_SHADER_STAGE_FRAGMENT_BIT>, // clusterIndexList
	Vk::SampledImageSlot<13, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // texAmbient (Pass 1 Output)
	Vk::SamplerSlot<14, VK_SHADER_STAGE_FRAGMENT_BIT>,		 // pointSampler
	EngineAS<15, VK_SHADER_STAGE_FRAGMENT_BIT>,				 // TLAS
	Vk::SampledImageSlot<16, VK_SHADER_STAGE_FRAGMENT_BIT>,	 // punctualShadowCube (Point)
	Vk::SampledImageSlot<17, VK_SHADER_STAGE_FRAGMENT_BIT>	 // punctualShadow2D (Spot)
	>;

using ReflectionLayout =
	Vk::DescriptorLayout<Vk::SampledImageSlot<0>,						   // texAlbedo
						 Vk::SamplerSlot<1>,							   // sampler
						 Vk::SampledImageSlot<2>,						   // texDepth
						 Vk::SampledImageSlot<3>,						   // texNormalRoughness
						 Vk::SamplerSlot<4>,							   // pointSampler
						 Vk::SampledImageSlot<5>,						   // texEnvMap
						 EngineAS<6, VK_SHADER_STAGE_FRAGMENT_BIT>,		   // TLAS
						 Vk::UniformSlot<7, VK_SHADER_STAGE_FRAGMENT_BIT>, // frame
						 Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>, // brdfLUT
						 Vk::SamplerSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>,		// clampSampler
						 Vk::SampledImageSlot<10, VK_SHADER_STAGE_FRAGMENT_BIT> // texLighting (Pass
																				// 2 Output)
						 >;

using BakeLayout = Vk::DescriptorLayout<Vk::StorageImageSlot<0>>;

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
struct BloomPass {
	static constexpr std::string_view name = "[GPU] Bloom Pass";
};
struct BlitPass {
	static constexpr std::string_view name = "[GPU] Blit/Composite";
};
struct PostProcessPass {
	static constexpr std::string_view name = "[GPU] PostProcess (GI)";
};
} // namespace Stages

using FrameProfiler =
	Profiler::GpuProfiler<Stages::ShadowPass, Stages::MainPass, Stages::AAPass,
						  Stages::PostProcessPass, Stages::BloomPass, Stages::BlitPass>;

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

struct ShaderStageSource {
	const char* path;
	std::span<const std::uint8_t> fallback;
	const char* entryPoint = "main";
};

struct NativeMaterial {
	Vk::Pipeline pipeline;
	Vk::PipelineLayout layout;
};

static constexpr uint32_t kGpuCullingMaxInstances = 8192;
static constexpr uint32_t kGpuCullingMaxBatches = 256;
static constexpr uint32_t kGpuCullingMaxVisibleInstances =
	kGpuCullingMaxInstances * kGpuCullingMaxBatches;

/**
 * @brief Heavily optimized, cache-line-friendly CPU draw command.
 * All redundant texture indexing and geometric properties have been stripped [3].
 */
struct DrawCommand {
	InstanceData
		instanceData; // 272 bytes (contains world, prevWorld, indexes, factor overrides, etc.)
	NativeMaterial* material;		   // 8 bytes
	NativeMesh* posMesh;			   // 8 bytes
	NativeMesh* attrMesh;			   // 8 bytes
	NativeMesh* skinMesh;			   // 8 bytes
	BufferHandle skinnedVertexBuffer;  // 8 bytes
	uint32_t jointOffset;			   // 4 bytes
	uint32_t morphOffset;			   // 4 bytes
	uint32_t activeMorphCount;		   // 4 bytes
	std::array<float, 4> morphWeights; // 16 bytes
	DrawFlags flags;				   // 4 bytes
};

static_assert(std::is_trivially_copyable_v<DrawCommand> &&
			  std::is_trivially_constructible_v<DrawCommand>);

struct UIDrawCommand {
	NativeMesh* posMesh;
	NativeMesh* attrMesh;
	uint32_t fontIndex;

	// --- Scissoring Bounds ---
	bool useScissor = false;
	ScissorRect scissorRect{};
};

struct WorkerCmdContext {
	std::array<Vk::CommandPool, 2> pools;
	std::array<ZHLN::Atomic<uint32_t>, 2> cmdCount{};
};

template <VkImageLayout ColorL, VkImageLayout DepthL> struct SceneResources {
	Vk::TypedImage<ColorL> sceneColor;
	Vk::TypedImage<ColorL> velocity;
	Vk::TypedImage<ColorL> normRough;
	Vk::TypedImage<DepthL> depth;
};

// --- Impl Struct ---

struct RenderContext::Impl {
	struct RenderState {
		SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
			initialState;
		Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> finalColor;
		Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> bloomFinal;
		SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
			resourcesForAA;
		SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
			aaResult;
	};
	Window& window;
	String64 appName;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;
	Vk::PresentationContext presentation;
	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;

	// These declarations are now mathematically tied to ActiveGBuffer
	Vk::RenderTarget<ActiveGBuffer::get<0>()> sceneColor;
	Vk::RenderTarget<ActiveGBuffer::get<1>()> velocityBuffer;
	Vk::RenderTarget<ActiveGBuffer::get<2>()> normalRoughnessBuffer;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> ambientTarget;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> lightingTarget;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> postProcessTarget;
	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomThresholdTarget;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomBlurTarget;
	Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomFinalTarget;

	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<FXAALayout> fxaaPass;
	Vk::PostProcessPass<SMAAEdgeLayout> smaaEdgePass;
	Vk::PostProcessPass<SMAAWeightLayout> smaaWeightPass;
	Vk::PostProcessPass<SMAABlendLayout> smaaBlendPass;

	uint32_t smaaAreaTexIdx = 0;
	uint32_t smaaSearchTexIdx = 0;

	Vk::PostProcessPass<AmbientLayout> ambientPass;
	Vk::PostProcessPass<LightingLayout> lightingPass;
	Vk::PostProcessPass<ReflectionLayout> reflectionPass;
	Vk::PostProcessPass<BlitLayout> blitPass;
	Vk::PostProcessPass<BloomThresholdLayout> bloomThresholdPass;
	Vk::PostProcessPass<BlurLayout> bloomBlurHPass;
	Vk::PostProcessPass<BlurLayout> bloomBlurVPass;

	Vk::RenderTarget<VK_FORMAT_R8G8_UNORM> smaaEdgeTarget;
	Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> smaaWeightTarget;

	Vk::ComputePass clusterBoundsPass;
	Vk::ComputePass clusterCullingPass;

	Vk::DescriptorSetLayout clusterCullingDescLayout;
	Vk::DescriptorPool clusterCullingPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> clusterCullingSets;

	Vk::Buffer clusterBoundsBuffer;
	ZHLN::DoubleBuffered<Vk::Buffer> clusterGridBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightIndexListBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> globalCounterBuffers;

	Vk::Sampler defaultSampler;
	Vk::Sampler pointSampler;

	Vk::ComputePass proceduralBakePass;
	Vk::DescriptorSetLayout proceduralBakeDescLayout;
	Vk::DescriptorPool proceduralBakeDescPool;
	VkDescriptorSet proceduralBakeSet{};

	uint32_t frame_index = 0;
	bool resized = true;
	bool needsInitialClear = true;
	VkCommandBuffer current_cmd = VK_NULL_HANDLE;
	uint32_t current_image_index = 0;

	JPH::Mat44 current_view_proj{};
	JPH::Mat44 unjittered_view_proj{};
	JPH::Mat44 shadowProjView{};
	FrameUniforms currentUniforms{};

	// Generational pools (Expanded to provide massive headroom for SoA split streams)
	GenerationalPool<NativeMesh, 8192, BufferHandle> meshPool;
	GenerationalPool<NativeMaterial, 2048, PipelineHandle> materialPool;

	ZHLN::Array<DrawCommand> drawQueue;
	ZHLN::Array<WorkerCmdContext> workerCmds;

	bool depth_ready = false;
	Vk::DescriptorPool uiPool;

	Vk::DescriptorSetLayout bindlessLayout;
	Vk::DescriptorPool bindlessPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> bindlessSets;
	Vk::Sampler globalSampler;

	uint32_t nextTextureIndex = 0;
	ZHLN::Array<Vk::Image> textureImages;
	ZHLN::Array<Vk::ImageView> textureViews;

	Vk::DescriptorSetLayout cullingLayout;
	Vk::DescriptorPool cullingPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> cullingSets;

	static constexpr uint32_t SHADOW_RES = 2048;
	static constexpr uint32_t NUM_CASCADES = 4;

	Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMap;
	Vk::Sampler shadowSampler;
	Vk::Sampler clampSampler;

	ZHLN::Array<Vk::ImageView> shadowCascadeViews;

	Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowAtlas;
	Vk::ImageView shadowAtlasCubeView;
	Vk::ImageView shadowAtlas2DView;

	ZHLN::Array<Vk::ImageView> punctualShadowViews;
	ZHLN::Array<GPULight> mappedLights;

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
	DoubleBuffered<Vk::Buffer> tlasInstanceBuffers;
	DoubleBuffered<Vk::Buffer> tlasStagingBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> frameUniformBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightStorageBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> instanceDataBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> indirectCommandsBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> shadowIndirectBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> jointBuffers; // Global Joint Transforms SSBO
	Vk::TypedPipeline<0, true> shadowPipeline;
	Vk::PipelineLayout shadowPipelineLayout;
	Vk::TypedPipeline<0, true> punctualShadowPipeline;
	Vk::PipelineLayout punctualShadowPipelineLayout;
	Vk::ComputePass cullingPass;

	ZHLN::Array<UIDrawCommand> uiDrawQueue;
	Vk::Pipeline uiPipeline;
	Vk::PipelineLayout uiPipelineLayout;

	GISettings giSettings{};

	Vk::Buffer morphDeltasBuffer; // Holds all packed morph target deltas
	uint32_t nextMorphDeltaIndex = 0;

	ZHLN::DoubleBuffered<BufferHandle> debugMeshHandles;

	AAState aaState{};

	FrameProfiler gpuProfiler;

	Vk::ComputePass skinningPass;
	Vk::PipelineLayout skinningPipelineLayout;

	void InitSubsystems(const RenderConfig& cfg, int width, int height);

	struct alignas(8) SkinningConstants {
		uint64_t inPosAddr;
		uint64_t inAttrAddr;
		uint64_t inSkinAddr;
		uint64_t outPosAddr;
		uint64_t outAttrAddr;
		uint64_t jointsAddr;
		uint64_t morphDeltasAddr;
		uint32_t vertexCount;
		uint32_t jointOffset;
		uint32_t morphOffset;
		uint32_t activeMorphCount;
		float morphWeights[4];
	};

	struct BakePush {
		uint32_t width;
		uint32_t height;
		float scale;
		float randomness;
		float distortion;
		uint32_t bakeType;
	};

	struct BlitPushConstants {
		float vignetteIntensity;
		float vignettePower;
		int fullBright;
	};

	Vk::ComputePass hangGpuPass;

	void BuildHangGpuPipeline();

	struct PipelineRegistration {
		const char* name;
		std::function<void()> build;
		std::vector<const char*> watchPaths;
	};

	void RegisterPipeline(const PipelineRegistration& reg) noexcept;

	void ProvokeDeviceLostInternal() const;

	void BuildSkinningPipeline();
	void DispatchSkinningPasses();

	void BuildProceduralBakePipeline();
	uint32_t BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx,
								   float scale, float randomness, float distortion);

	Impl(Window& win) : window(win) {}

	bool hasSkinnedThisFrame = false;

	void InitialClearTargets(VkCommandBuffer cmd) noexcept;
	void BuildTLAS(VkCommandBuffer cmd) noexcept;

	void InitShadowResources();
	void InitCullingResources();
	void CompileShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
	void CompilePunctualShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
	void InitBindless();
	void BuildTAAPipeline();
	void BuildFXAAPipeline();
	void BuildSMAAPipeline();
	void BuildAmbientPipeline();
	void BuildLightingPipeline();
	void BuildReflectionPipelines();
	void BuildBlitPipeline();
	void BuildBloomPipelines();
	void InitPostProcessing();
	void extracted();
	void SetupUI(GLFWwindow* window);

	// Core Vulkan allocation implementation
	uint32_t CreateTextureInternal(const void* data, uint32_t width, uint32_t height, bool isSRGB);
	uint32_t CreateTextureCubeInternal(const void* const* faceData, uint32_t width,
									   uint32_t height);

	auto CreateGPUBuffer(size_t size, const void* data, VkBufferUsageFlags functionalUsage) const
		-> std::pair<Vk::Buffer, VkDeviceAddress>;

	void SortDrawQueue();
	ZHLN_FrameResult SubmitFrame();
	void InitializeSystemTextures();

	// Persistent, reusable memory block to prevent dynamic heap reallocations
	ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;

	template <bool FullBright> void RecordSceneFrame(VkCommandBuffer cmd);

	~Impl() {
		if (ctx.Device() != VK_NULL_HANDLE) {
			for (uint32_t i = 0; i < 2; ++i) {
				if (tlas[i] != VK_NULL_HANDLE) {
					rtCtx.DestroyAS(tlas[i]);
				}
			}
		}
	}

	struct WatchableShader {
		std::string path;
		FileWatcher watcher;
		std::function<void()> reloadCallback;
	};

	ZHLN::Array<WatchableShader> shaderWatchers;

	void RegisterShaderWatcher(const char* path, std::function<void()> callback);

	void CheckShaderWatchers() noexcept {
		if constexpr (isDev) {
			bool anyReloaded = false;
			for (auto& watcher : shaderWatchers) {
				if (watcher.watcher.CheckModified()) {
					if (!anyReloaded) {
						// Prevent write-after-read race conditions by forcing GPU idle
						vkDeviceWaitIdle(ctx.Device());
						anyReloaded = true;
					}
					watcher.reloadCallback();
				}
			}
		}
	}

	template <VkFormat F>
	auto CreateDefaultTarget(VkExtent2D ext, VkImageUsageFlags extraFlags = 0) {
		return Vk::RenderTarget<F>::Create(allocator, ctx, ext,
										   {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
													 VK_IMAGE_USAGE_SAMPLED_BIT | extraFlags});
	}

	bool RecreateTargets(VkExtent2D ext);

	static constexpr uint32_t MAX_PUNCTUAL_LIGHTS = 4;

	void RecreatePunctualShadowViews() noexcept;
	void InitSkeletalAnimationResources();
	void InitLightingLUTs();

	[[nodiscard]] Vk::ShaderStages LoadAndCreateShaders(ShaderStageSource vs,
														ShaderStageSource ps) const noexcept;

	[[nodiscard]] Vk::Pipeline LoadAndCreateComputeShader(ShaderStageSource cs,
														  VkPipelineLayout layout) const noexcept;

	void WatchPipeline(const char* vsPath, const char* psPath,
					   std::function<void()> rebuild_fn) noexcept;
};

// --- Promoted G-Buffer & Post-Process Views ---

struct GBufferView {
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> sceneColor;
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth;
	Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> normRough;
};

struct PostProcessResources {
	GBufferView gbuffer;
	const Vk::Buffer& frameUniforms;
	const Vk::Sampler& defaultSampler;
	const Vk::Sampler& pointSampler;
	const Vk::Sampler& clampSampler;
};

struct FrameRecorder {
	VkCommandBuffer cmd;
	RenderContext::Impl& ctx;
	uint32_t frameIndex;
	VkDescriptorSet bindlessSet;

	FrameRecorder(VkCommandBuffer c, RenderContext::Impl& impl) noexcept
		: cmd(c), ctx(impl), frameIndex(impl.frame_index),
		  bindlessSet(impl.bindlessSets[impl.frame_index]) {}
};

struct GroupRange {
	NativeMaterial* material;
	uint32_t start;
	uint32_t count;
};

// --- Render Pass Concept Validation ---

template <typename T, typename... Args>
concept IsRenderPass = requires(T pass, Args&&... args) {
	{ pass.Execute(std::forward<Args>(args)...) };
};

template <typename Pass, typename... Args>
	requires IsRenderPass<Pass, Args...>
void RunPass(const Pass& pass, Args&&... args) {
	pass.Execute(std::forward<Args>(args)...);
}

// --- Render Pass Specifications ---

namespace Passes {

struct ShadowPass {
	static constexpr uint32_t kCubemapFaceMask = 0x3F;
	static constexpr float kShadowClearDepth = 1.0f;
	void Execute(const FrameRecorder& recorder) const noexcept;
};

struct MainPass {
	void Execute(const FrameRecorder& recorder,
				 SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>
					 in) const noexcept;
};

struct DeferredLightingPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> in) const noexcept
		-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

  private:
	[[nodiscard]] constexpr uint32_t DetermineLightingVariant(const GISettings& gi,
															  bool hasRt) const noexcept;
	[[nodiscard]] constexpr uint32_t DetermineReflectionVariant(const GISettings& gi,
																bool hasRt) const noexcept;
};

struct ForwardPass {
	void Execute(const FrameRecorder& recorder,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> litColor,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> depth) const noexcept;
};

struct BloomPass {
	[[nodiscard]] auto
	Execute(const FrameRecorder& recorder,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor) const noexcept
		-> Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
};

struct AAPass {
	using SceneRO = SceneResources<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
								   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;
	using ColorImageRO = Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

	[[nodiscard]] auto Execute(const FrameRecorder& recorder, SceneRO in) const noexcept -> SceneRO;

  private:
	[[nodiscard]] auto ExecuteTAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
								  const SceneRO& in, ColorImageRO color_ro) const noexcept
		-> ColorImageRO;
	[[nodiscard]] auto ExecuteFXAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
								   const SceneRO& in, ColorImageRO color_ro) const noexcept
		-> ColorImageRO;
	[[nodiscard]] auto ExecuteSMAA(VkCommandBuffer cmd, const FrameRecorder& recorder,
								   const SceneRO& in, ColorImageRO color_ro) const noexcept
		-> ColorImageRO;
};

struct BlitPass {
	void
	Execute(const FrameRecorder& recorder,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
			Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> bloomColor) const noexcept;
};

} // namespace Passes

inline std::vector<uint32_t> LoadShaderSpv(const std::string& path) noexcept {
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file.is_open()) {
		return {};
	}
	auto fileSize = (size_t)file.tellg();
	std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
	file.close();
	return buffer;
}

inline bool LoadShaderData(const ShaderStageSource& src, const void*& outData, size_t& outSize,
						   std::vector<uint32_t>& diskBuffer) {
	outData = src.fallback.data();
	outSize = src.fallback.size_bytes();
	if constexpr (isDev) {
		diskBuffer = LoadShaderSpv(src.path);
		if (!diskBuffer.empty()) {
			outData = diskBuffer.data();
			outSize = diskBuffer.size() * 4;
			return true;
		}
	}
	return false;
}

template <typename T = Vk::Buffer, typename... Args>
DoubleBuffered<T> CreateDoubleBuffered(Vk::Allocator& alloc, Args&&... args) {
	return DoubleBuffered<T>{T::Create(alloc.Get(), std::forward<Args>(args)...),
							 T::Create(alloc.Get(), std::forward<Args>(args)...)};
}

} // namespace ZHLN

template <> struct ZHLN::Vk::FormatOf<float[3]> {
	static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <> struct ZHLN::Vk::FormatOf<::ZHLN::Packed1010102> {
	static constexpr auto value = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
};
template <> struct ZHLN::Vk::FormatOf<::ZHLN::PackedHalf2> {
	static constexpr auto value = VK_FORMAT_R16G16_SFLOAT;
};
template <> struct ZHLN::Vk::FormatOf<::ZHLN::PackedRGBA8> {
	static constexpr auto value = VK_FORMAT_R8G8B8A8_UNORM;
};

ZHLN_REFLECT_VERTEX(::ZHLN::VertexPosition, position);
ZHLN_REFLECT_VERTEX(::ZHLN::VertexAttributes, normal, tangent, uv, color);
ZHLN_REFLECT_VERTEX(::ZHLN::VertexSkin, joints, weights);
