// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderInternal.hpp
#pragma once
#include "Rendering.hpp"
#include "detail/Array.hpp"
#include "detail/ControlFlow.hpp"
#include "detail/RadixSort.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Resources.hpp"
#include "threading/Mutex.hpp"

#include <GLFW/glfw3.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <cstddef>
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
	Vk::UniformSlot<2, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT>, // FrameUniforms
																				   // (UBO)
	Vk::StorageBufferSlot<3, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT>, // Lights
																						 // (SSBO)
	Vk::StorageBufferSlot<4, VK_SHADER_STAGE_VERTEX_BIT |
								 VK_SHADER_STAGE_FRAGMENT_BIT>, // Instance buffer (SSBO)
	Vk::StorageBufferSlot<5, VK_SHADER_STAGE_VERTEX_BIT>,		// Joint matrices (SSBO)
	Vk::StorageBufferSlot<6, VK_SHADER_STAGE_VERTEX_BIT>,		// Previous Joint matrices (SSBO)
	Vk::StorageBufferSlot<7, VK_SHADER_STAGE_VERTEX_BIT>,		// Morph target deltas (SSBO)
	Vk::SampledImageSlot<8, VK_SHADER_STAGE_FRAGMENT_BIT>, // Pre-filtered Cubemap (Specular IBL)
	Vk::SampledImageSlot<9, VK_SHADER_STAGE_FRAGMENT_BIT>, // 2D BRDF LUT (2D Texture)
	Vk::SamplerSlot<10, VK_SHADER_STAGE_FRAGMENT_BIT>	   // Clamping Sampler
	>;

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

using KawaseLayout =
	Vk::DescriptorLayout<Vk::SampledImageSlot<0>, // texInput (current source)
						 Vk::SamplerSlot<1>,	  // sampler
						 Vk::SampledImageSlot<2>  // texLow (downsampled source for combine)
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
struct BloomThreshPass {
	static constexpr std::string_view name = "[GPU] Bloom Threshold";
};
struct BloomBlurHPass {
	static constexpr std::string_view name = "[GPU] Bloom Blur H";
};
struct BloomBlurVPass {
	static constexpr std::string_view name = "[GPU] Bloom Blur V";
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
						  Stages::PostProcessPass, Stages::BloomThreshPass, Stages::BloomBlurHPass,
						  Stages::BloomBlurVPass, Stages::BlitPass>;

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
	std::array<Vk::CommandPool<Vk::QueueType::Graphics>, 2> pools;
	std::array<ZHLN::Atomic<uint32_t>, 2> cmdCount{};
};

template <VkImageLayout ColorL, VkImageLayout DepthL> struct SceneResources {
	Vk::TypedImage<ColorL> sceneColor;
	Vk::TypedImage<ColorL> velocity;
	Vk::TypedImage<ColorL> normRough;
	Vk::TypedImage<DepthL> depth;
};

// ============================================================================
// Frame Graph Resource Tags (Declared Before PIMPL Struct)
// ============================================================================
using Res_SceneColor =
	Vk::GraphImage<"SceneColor", VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Velocity = Vk::GraphImage<"Velocity", VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_NormRough =
	Vk::GraphImage<"NormRough", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Depth = Vk::GraphImage<"Depth", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowMap = Vk::GraphImage<"ShadowMap", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_ShadowAtlas =
	Vk::GraphImage<"ShadowAtlas", VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT>;
using Res_Ambient =
	Vk::GraphImage<"Ambient", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Lighting =
	Vk::GraphImage<"Lighting", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_PostProcess =
	Vk::GraphImage<"PostProcess", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomThresh =
	Vk::GraphImage<"BloomThresh", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown1 =
	Vk::GraphImage<"BloomDown1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown2 =
	Vk::GraphImage<"BloomDown2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomDown3 =
	Vk::GraphImage<"BloomDown3", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomUp2 =
	Vk::GraphImage<"BloomUp2", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomUp1 =
	Vk::GraphImage<"BloomUp1", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_BloomFinal =
	Vk::GraphImage<"BloomFinal", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_AccumCurr =
	Vk::GraphImage<"AccumCurr", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_AccumNext =
	Vk::GraphImage<"AccumNext", VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaEdge = Vk::GraphImage<"SmaaEdge", VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_SmaaWeight =
	Vk::GraphImage<"SmaaWeight", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT>;
using Res_Swapchain =
	Vk::GraphImage<"Swapchain", VK_FORMAT_B8G8R8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, true>;

// --- Impl Struct ---

struct RenderContext::Impl {
	// ============================================================================
	// Nested Types & Reflection Metadata
	// ============================================================================
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

	struct GraphResources {
		Vk::RenderTarget<VK_FORMAT_B10G11R11_UFLOAT_PACK32> sceneColor;
		Vk::RenderTarget<VK_FORMAT_R16G16_SFLOAT> velocityBuffer;
		Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> normalRoughnessBuffer;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> ambientTarget;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> lightingTarget;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> postProcessTarget;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomThresholdTarget;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomDown1;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomDown2;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomDown3;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomUp2;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomUp1;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomFinalTarget;
		Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT> bloomBlurTarget;
		Vk::RenderTarget<VK_FORMAT_R8G8_UNORM> smaaEdgeTarget;
		Vk::RenderTarget<VK_FORMAT_R8G8B8A8_UNORM> smaaWeightTarget;
		Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowMap;
		Vk::RenderTarget<VK_FORMAT_D32_SFLOAT> shadowAtlas;

		struct ReflectMetadata {
			Res_SceneColor sceneColor;
			Res_Velocity velocityBuffer;
			Res_NormRough normalRoughnessBuffer;
			Res_Ambient ambientTarget;
			Res_Lighting lightingTarget;
			Res_PostProcess postProcessTarget;
			Res_BloomThresh bloomThresholdTarget;
			Res_BloomDown1 bloomDown1;
			Res_BloomDown2 bloomDown2;
			Res_BloomDown3 bloomDown3;
			Res_BloomUp2 bloomUp2;
			Res_BloomUp1 bloomUp1;
			Res_BloomFinal bloomFinalTarget;
			Res_SmaaEdge smaaEdgeTarget;
			Res_SmaaWeight smaaWeightTarget;
			Res_ShadowMap shadowMap;
			Res_ShadowAtlas shadowAtlas;
		};
	};

	static constexpr uint32_t SHADOW_RES = 2048;
	static constexpr uint32_t NUM_CASCADES = 4;
	static constexpr uint32_t MAX_PUNCTUAL_LIGHTS = 4;

	// ============================================================================
	// Core System Properties (64-Bit / High Alignment)
	// ============================================================================
	Window& window;
	String64 appName;
	Vk::Context ctx;
	Vk::Allocator allocator;
	Vk::Surface surface;
	Vk::PresentationContext presentation;
	Vk::FrameSync<2> sync;
	Vk::CommandPools<2> pools;
	Vk::StagingRingBuffer stagingRingBuffer;
	mutable Vk::StagingRingBuffer transferRingBuffer;

	mutable Vk::CommandRing<Vk::QueueType::Graphics, 8> graphicsCmdRing;
	mutable Vk::CommandRing<Vk::QueueType::Transfer, 8> transferCmdRing;

	VkCommandBuffer current_cmd = VK_NULL_HANDLE;

	// ============================================================================
	// Staging, Memory Reclamation & Multithreading
	// ============================================================================
	std::unique_ptr<Vk::StagingContext> stagingContext;
	Vk::DeletionQueue deletionQueue;
	std::optional<Vk::ScopedDeletionQueue> activeQueueGuard;

	ZHLN::Array<WorkerCmdContext> workerCmds;
	DoubleBuffered<Vk::ParallelCommandRecorder<2>> parallelRecorder;

	struct PendingAcquires {
		ZHLN::Mutex mutex{};
		ZHLN::Array<VkBufferMemoryBarrier2> buffers;

		void Drain(VkCommandBuffer cmd) noexcept {
			ZHLN_LOCK(mutex) {
				if (!buffers.empty()) {
					VkDependencyInfo depInfo = {
						.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
						.pNext = {},
						.dependencyFlags = {},
						.memoryBarrierCount = {},
						.pMemoryBarriers = {},
						.bufferMemoryBarrierCount = static_cast<uint32_t>(buffers.size()),
						.pBufferMemoryBarriers = buffers.data(),
						.imageMemoryBarrierCount = {},
						.pImageMemoryBarriers = {},
					};
					vkCmdPipelineBarrier2(cmd, &depInfo);
					buffers.clear();
				}
			}
		}
	};
	mutable PendingAcquires pendingAcquires;

	// ============================================================================
	// Reflected Graph Resources & Backward-Compatible Aliases
	// ============================================================================
	GraphResources graphResources;

	DoubleBuffered<Vk::RenderTarget<VK_FORMAT_R16G16B16A16_SFLOAT>> accumBuffers;

	// ============================================================================
	// Textures, Heaps & Bindless Descriptors
	// ============================================================================
	Vk::DescriptorSetLayout bindlessLayout;
	Vk::DescriptorPool bindlessPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> bindlessSets;

	Vk::Sampler globalSampler;
	Vk::Sampler clampSampler;
	Vk::Sampler defaultSampler;
	Vk::Sampler pointSampler;

	ZHLN::Array<Vk::Image> textureImages;
	ZHLN::Array<Vk::ImageView> textureViews;

	// ============================================================================
	// Pipeline Objects & Compute Passes
	// ============================================================================
	Vk::PostProcessPass<TAALayout> taaPass;
	Vk::PostProcessPass<FXAALayout> fxaaPass;
	Vk::PostProcessPass<SMAAEdgeLayout> smaaEdgePass;
	Vk::PostProcessPass<SMAAWeightLayout> smaaWeightPass;
	Vk::PostProcessPass<SMAABlendLayout> smaaBlendPass;

	Vk::PostProcessPass<AmbientLayout> ambientPass;
	Vk::PostProcessPass<LightingLayout> lightingPass;
	Vk::PostProcessPass<ReflectionLayout> reflectionPass;
	Vk::PostProcessPass<BlitLayout> blitPass;
	Vk::PostProcessPass<BloomThresholdLayout> bloomThresholdPass;
	std::array<Vk::PostProcessPass<KawaseLayout>, 3> bloomDownPass;
	std::array<Vk::PostProcessPass<KawaseLayout>, 3> bloomUpPass;

	Vk::ComputePass clusterBoundsPass;
	Vk::ComputePass clusterCullingPass;
	Vk::ComputePass cullingPass;
	Vk::ComputePass skinningPass;
	Vk::ComputePass proceduralBakePass;
	Vk::ComputePass hangGpuPass;

	Vk::PipelineLayout skinningPipelineLayout;
	Vk::PipelineLayout shadowPipelineLayout;
	Vk::PipelineLayout punctualShadowPipelineLayout;

	Vk::TypedPipeline<0, true> shadowPipeline;
	Vk::TypedPipeline<0, true> punctualShadowPipeline;

	// ============================================================================
	// GPU Storage & Double-Buffered Work Buffers
	// ============================================================================
	Vk::Buffer clusterBoundsBuffer;
	ZHLN::DoubleBuffered<Vk::Buffer> clusterGridBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightIndexListBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> globalCounterBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> frameUniformBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> lightStorageBuffers;

	ZHLN::DoubleBuffered<Vk::Buffer> instanceDataBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> indirectCommandsBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> shadowIndirectBuffers;
	ZHLN::DoubleBuffered<Vk::Buffer> jointBuffers;
	Vk::Buffer morphDeltasBuffer;

	// ============================================================================
	// Descriptor Sets, Culling, Shadows & Lighting LUTs
	// ============================================================================
	Vk::DescriptorSetLayout cullingLayout;
	Vk::DescriptorPool cullingPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> cullingSets;

	Vk::DescriptorSetLayout clusterCullingDescLayout;
	Vk::DescriptorPool clusterCullingPool;
	ZHLN::DoubleBuffered<VkDescriptorSet> clusterCullingSets;

	Vk::DescriptorSetLayout proceduralBakeDescLayout;
	Vk::DescriptorPool proceduralBakeDescPool;
	VkDescriptorSet proceduralBakeSet = VK_NULL_HANDLE;

	ZHLN::Array<Vk::ImageView> shadowCascadeViews;
	Vk::ImageView shadowAtlasCubeView;
	Vk::ImageView shadowAtlas2DView;
	ZHLN::Array<Vk::ImageView> punctualShadowViews;
	Vk::Sampler shadowSampler;

	Vk::Image ltcMatImage;
	Vk::ImageView ltcMatView;
	Vk::Image ltcAmpImage;
	Vk::ImageView ltcAmpView;

	Vk::IBLPayload iblPayload;

	// ============================================================================
	// Generational Memory Pools & Draw Commands
	// ============================================================================
	GenerationalPool<NativeMesh, 8192, BufferHandle> meshPool;
	GenerationalPool<NativeMaterial, 2048, PipelineHandle> materialPool;

	ZHLN::Array<DrawCommand> drawQueue;
	ZHLN::Array<GPULight> mappedLights;
	ZHLN::DoubleBuffered<BufferHandle> debugMeshHandles;

	// ============================================================================
	// User Interface Rendering (Vulkan Bound)
	// ============================================================================
	Vk::DescriptorPool uiPool;
	Vk::Pipeline uiPipeline;
	Vk::PipelineLayout uiPipelineLayout;
	ZHLN::Array<UIDrawCommand> uiDrawQueue;

	// ============================================================================
	// Hardware Ray Tracing Context (TLAS)
	// ============================================================================
	Vk::RayTracingContext rtCtx;
	DoubleBuffered<VkAccelerationStructureKHR> tlas;
	DoubleBuffered<Vk::Buffer> tlasBuffer;
	DoubleBuffered<Vk::Buffer> tlasScratchBuffer;
	DoubleBuffered<Vk::Buffer> tlasInstanceBuffers;
	DoubleBuffered<Vk::Buffer> tlasStagingBuffers;

	// ============================================================================
	// Local Configuration & Math Tracking States
	// ============================================================================
	JPH::Mat44 current_view_proj = JPH::Mat44::sIdentity();
	JPH::Mat44 unjittered_view_proj = JPH::Mat44::sIdentity();
	JPH::Mat44 shadowProjView = JPH::Mat44::sIdentity();
	FrameUniforms currentUniforms{};

	GISettings giSettings{};
	AAState aaState{};
	FrameProfiler gpuProfiler;

	// ============================================================================
	// Hot-Reloading & Code Watchers
	// ============================================================================
	struct WatchableShader {
		std::string path;
		FileWatcher watcher;
		std::function<void()> reloadCallback;
	};

	ZHLN::Array<WatchableShader> shaderWatchers;

	// ============================================================================
	// Primitives / State Boundaries (Grouped at Tail to Minimize Padding)
	// ============================================================================
	uint32_t frame_index = 0;
	uint32_t current_image_index = 0;
	uint32_t nextTextureIndex = 0;
	uint32_t nextMorphDeltaIndex = 0;
	uint32_t smaaAreaTexIdx = 0;
	uint32_t smaaSearchTexIdx = 0;

	float lastAspectRatio = 0.0f;
	float lastFov = 0.0f;

	bool resized = true;
	bool needsInitialClear = true;
	bool depth_ready = false;
	bool hasSkinnedThisFrame = false;

	// ============================================================================
	// Helper Structures & Temporary Stack Scratches
	// ============================================================================
	ZHLN::Array<VkAccelerationStructureInstanceKHR> tlasInstancesScratch;
	ZHLN::Array<SortItem> sortItemsScratch;
	ZHLN::Array<SortItem> sortTempScratch;
	ZHLN::Array<DrawCommand> sortDrawQueueScratch;

	// ============================================================================
	// API Signatures & Execution Hooks
	// ============================================================================
	Impl(Window& win) : window(win) {}

	~Impl() {
		graphicsCmdRing.Cleanup();
		transferCmdRing.Cleanup();
		if (ctx.Device() != VK_NULL_HANDLE) {
			for (uint32_t i = 0; i < 2; ++i) {
				if (tlas[i] != VK_NULL_HANDLE) {
					rtCtx.DestroyAS(tlas[i]);
				}
			}
		}
	}

	[[nodiscard]] std::expected<void, std::string> InitSubsystems(const RenderConfig& cfg,
																  int width, int height);

	void FlipSubsystems() noexcept;

	struct PPPushConstants {
		JPH::Mat44 invViewProj;
		JPH::Mat44 viewProj;
		alignas(16) std::array<float, 4> camPos;
		int giMode;
		float aoRadius;
		float aoBias;
		float aoPower;
		float giIntensity;
		int giSamples;
		int enableSSR;
		int enableRTR;
		int _pad;
	};

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

	struct PipelineRegistration {
		const char* name;
		std::function<void()> build;
		std::vector<const char*> watchPaths;
	};

	void RegisterPipeline(const PipelineRegistration& reg) noexcept;
	void ProvokeDeviceLostInternal() const;

	[[nodiscard]] std::expected<void, std::string> BuildSkinningPipeline();
	void DispatchSkinningPasses();

	void BuildProceduralBakePipeline();
	uint32_t BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx,
								   float scale, float randomness, float distortion);

	void BuildTLAS(VkCommandBuffer cmd) noexcept;

	void InitShadowResources();
	[[nodiscard]] std::expected<void, std::string> InitCullingResources();
	[[nodiscard]] std::expected<void, std::string>
	CompileShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
	[[nodiscard]] std::expected<void, std::string>
	CompilePunctualShadowPipeline(VkDevice device, const Resource::ShaderPair& shaderData);
	void InitBindless();
	[[nodiscard]] std::expected<void, std::string> BuildTAAPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildFXAAPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildSMAAPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildAmbientPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildLightingPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildReflectionPipelines();
	[[nodiscard]] std::expected<void, std::string> BuildBlitPipeline();
	[[nodiscard]] std::expected<void, std::string> BuildBloomPipelines();
	[[nodiscard]] std::expected<void, std::string> BuildHangGpuPipeline();
	[[nodiscard]] std::expected<void, std::string> InitPostProcessing();
	[[nodiscard]] std::expected<void, std::string> SetupUI(GLFWwindow* window);

	uint32_t CreateTextureInternal(const void* data, uint32_t width, uint32_t height, bool isSRGB);
	uint32_t CreateTextureCubeInternal(const void* const* faceData, uint32_t width, uint32_t height);

	auto CreateGPUBuffer(size_t size, const void* data, VkBufferUsageFlags functionalUsage) const
		-> std::pair<Vk::Buffer, VkDeviceAddress>;

	void SortDrawQueue();
	void InitializeSystemTextures();

	template <bool FullBright>
	void RecordSceneFrame(Vk::CommandBuffer<Vk::QueueType::Graphics> cmd);

	void RegisterShaderWatcher(const char* path, std::function<void()> callback);
	void CheckShaderWatchers() noexcept;

	template <VkFormat F>
	auto CreateDefaultTarget(VkExtent2D ext, VkImageUsageFlags extraFlags = 0) {
		return Vk::RenderTarget<F>::Create(allocator, ctx, ext,
										   {.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
													 VK_IMAGE_USAGE_SAMPLED_BIT | extraFlags});
	}

	bool RecreateTargets(VkExtent2D ext);

	void RecreatePunctualShadowViews() noexcept;
	void InitSkeletalAnimationResources();
	void InitLightingLUTs();

	[[nodiscard]] Vk::ShaderStages LoadAndCreateShaders(ShaderStageSource vs,
														ShaderStageSource ps) const noexcept;

	[[nodiscard]] Vk::Pipeline LoadAndCreateComputeShader(ShaderStageSource cs,
														  VkPipelineLayout layout) const noexcept;

	void WatchPipeline(const char* vsPath, const char* psPath,
					   std::function<void()> rebuild_fn) noexcept;

	void UploadClusterBounds(const JPH::Mat44& proj);
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
	Vk::CommandBuffer<Vk::QueueType::Graphics> cmd;
	RenderContext::Impl& ctx;
	uint32_t frameIndex;
	VkDescriptorSet bindlessSet;

	FrameRecorder(Vk::CommandBuffer<Vk::QueueType::Graphics> c, RenderContext::Impl& impl) noexcept
		: cmd(c), ctx(impl), frameIndex(impl.frame_index),
		  bindlessSet(impl.bindlessSets[impl.frame_index]) {}

	FrameRecorder(VkCommandBuffer c, RenderContext::Impl& impl) noexcept
		: cmd({c}), ctx(impl), frameIndex(impl.frame_index),
		  bindlessSet(impl.bindlessSets[impl.frame_index]) {}
};

struct GroupRange {
	const NativeMaterial* material;
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
				 SceneResources<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>
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
				 Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> litColor,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL> depth) const noexcept;
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
	void Execute(const FrameRecorder& recorder,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> inColor,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> bloomColor,
				 Vk::TypedImage<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> swapchainTarget,
				 int fullBright) const noexcept;
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
