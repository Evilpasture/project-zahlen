// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Geometry2D.hpp> // Extent2D, Offset2D
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/GpuEnums.hpp> // LightType, ParticleAlignment (re-exported below)
#include <GeneratedGpuTypes.hpp> // Generated host structs (a build output; see tools/zshader/GpuTypes.cpp)
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN {

// --- High-Level Persistent Asset Identifiers
using AssetID    = uint64_t;
using MaterialID = uint64_t;

inline constexpr AssetID    InvalidAssetID    = 0;
inline constexpr MaterialID InvalidMaterialID = 0;

constexpr AssetID HashAssetID(std::string_view name) noexcept {
    return Hash64(name);
}

// --- Core Math/Spatial Types

struct ScissorRect {
    int32_t  x;
    int32_t  y;
    uint32_t width;
    uint32_t height;
};

// Semantic types to help the Renderer choose the right Vulkan Format
struct Packed1010102 {
    uint32_t data;
}; // Normals/Tangents
struct PackedHalf2 {
    uint32_t data;
}; // UVs (2x 16-bit floats)
struct PackedRGBA8 {
    uint32_t data;
}; // Color

struct VertexPosition {
    float position[3]; // 12B - Full precision
};

struct VertexAttributes {
    Packed1010102 normal;  // 4B  - 10-bit per axis
    Packed1010102 tangent; // 4B  - 10-bit + sign
    PackedHalf2   uv;      // 4B  - 16-bit UVs
    PackedRGBA8   color;   // 4B  - RGBA8
}; // 16B - Perfect alignment

struct VertexSkin {
    uint16_t    joints[4]; // 8B  - 16-bit Joint indices
    PackedRGBA8 weights;   // 4B  - 8-bit UNORM weights mapped to [0.0, 1.0]
}; // 12B

// Partitioning limits. These match the guaranteed VK_EXT_mesh_shader minimums
// (maxMeshOutputVertices >= 256, maxMeshOutputPrimitives >= 256) with plenty of
// headroom, and are also the meshoptimizer build parameters used by the cooker
// and the runtime glTF importer, so cooked and JIT meshlets stay identical.
inline constexpr uint32_t kMeshletMaxVertices  = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124; // multiple of 4 (meshoptimizer recommendation)
inline constexpr float    kMeshletConeWeight   = 0.5f;
// Meshlets handled by one task-shader workgroup (one payload slot each).
inline constexpr uint32_t kMeshletsPerTaskGroup = 32;
// Threads per mesh-shader workgroup (one vertex per thread, 2 prims per thread).
inline constexpr uint32_t kMeshShaderGroupSize = 64;
// NOLINTBEGIN(performance-enum-size)
enum class TextureHandle : uint64_t { Invalid = 0 };
enum class AudioHandle : uint64_t { Invalid = 0 };
enum class SynthHandle : uint64_t { Invalid = 0 };

enum class AudioWaveformType : uint8_t { Sine = 0, Square = 1, Triangle = 2, Sawtooth = 3 };
enum class AudioFilterType : uint8_t { LowPass = 0, HighPass = 1, BandPass = 2, Notch = 3 };
enum class AudioNoiseType : uint8_t { White = 0, Pink = 1, Brownian = 2 };
// NOLINTEND(performance-enum-size)
// NOTE: these are BINDLESS SLOT indices conceptually, but they are NOT valid
// TextureHandles. TextureHandle keys are hashed asset ids
// (TextureManager::RegisterUploaded -> HashAssetID), and the fallback slots are
// registered separately (RenderInternal.hpp: black 0, white 1, flat normal 2).
// Passing SystemTextures::White therefore logs "[Warning] TextureHandle 0x2 was
// not found in registry" and falls back to white anyway. Use
// TextureHandle::Invalid when you want the white fallback texture.
namespace SystemTextures {
inline constexpr TextureHandle Invalid    = TextureHandle(0);
inline constexpr TextureHandle Black      = TextureHandle(1);
inline constexpr TextureHandle White      = TextureHandle(2);
inline constexpr TextureHandle FlatNormal = TextureHandle(3);
} // namespace SystemTextures

struct UIBatch {
    TextureHandle texture              = TextureHandle::Invalid;
    uint32_t      bindlessTextureIndex = 0; // Non-zero bypasses TextureManager lookup (user bindless IDs)
    uint32_t      vertexStart          = 0;
    uint32_t      vertexCount          = 0;
    bool          useScissor           = false;
    bool          isSDF                = false;
    bool          useTextureColor      = false;
    ScissorRect   scissorRect          = {};
};

// Immutable 2D UI geometry payload extracted from Clay by
// `GUI::Context::EndFrame`. Plain data: the GUI subsystem neither knows nor
// inherits from the renderer, it just describes quads.
//
// The spans point into storage the producing GUI context owns until its next
// frame, so the payload must be handed to `RenderContext::RenderUI` in the
// same frame it was built.
struct UIDrawData {
    std::span<const UIBatch>          batches;
    std::span<const VertexPosition>   positions;
    std::span<const VertexAttributes> attributes;

    [[nodiscard]] constexpr bool Empty() const noexcept {
        return batches.empty() || positions.empty() || attributes.empty();
    }
};

struct alignas(16) GPUVolumetricVolume {
    JPH::Mat44 invTransform;
    JPH::Vec4  extentsAndType;   // xyz = extents, w = type (0=Box, 1=Sphere)
    JPH::Vec4  colorAndDensity;  // xyz = color, w = density
    JPH::Vec4  emissiveAndAniso; // xyz = emissive, w = anisotropy
};
static_assert(sizeof(GPUVolumetricVolume) == 112);

// --- Opaque Resource Handles
enum class BufferHandle : uint64_t { Invalid = 0 };
enum class PipelineHandle : uint64_t { Invalid = 0 };
enum class ResourceGroupHandle : uint64_t { Invalid = 0 };

static_assert(sizeof(BufferHandle) == 8);
static_assert(sizeof(PipelineHandle) == 8);
static_assert(sizeof(ResourceGroupHandle) == 8);
static_assert(sizeof(TextureHandle) == 8);

// Pixel rectangle of a render target (framebuffer pixels, top-left origin,
// like window coordinates).
struct ViewportRect {
    uint32_t x      = 0;
    uint32_t y      = 0;
    uint32_t width  = 0;
    uint32_t height = 0;
};

// Universal subresource reference to any renderable GPU target. Fully
// identifies a swapchain backbuffer, an offscreen texture, a cubemap face or a
// mip level, so a caller never has to say *what kind* of target it is asking
// for: it addresses a subresource and the renderer resolves it.
//
// The handle stays opaque. What the texture physically *is* -- extent, format,
// layer count -- is a property of its allocation inside src/vulkan, never a
// mirrored public enum, so adding a new destination (OpenXR eye, cubemap probe
// face, portal) needs no enumeration of view kinds here.
struct RenderAttachment {
    TextureHandle texture    = TextureHandle::Invalid;
    uint16_t      mipLevel   = 0;
    uint16_t      arrayLayer = 0; // Cubemap face (0..5) or texture array slice

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return texture != TextureHandle::Invalid;
    }

    explicit constexpr operator bool() const noexcept {
        return Valid();
    }
};

static_assert(sizeof(RenderAttachment) == 16);

struct Mesh {
    using enum BufferHandle;
    BufferHandle posBuffer   = Invalid;
    BufferHandle attrBuffer  = Invalid;
    BufferHandle skinBuffer  = Invalid;
    BufferHandle indexBuffer = Invalid;
    uint32_t     vertexCount = 0;
    uint32_t     indexCount  = 0;

    // --- VK_EXT_mesh_shader meshlet streams
    // The raw position/attribute/index buffers above are deliberately kept:
    // ray tracing BLAS builds (ZHLN_CmdBuildBlas) and the legacy vertex
    // pipeline still consume them. Meshlets are an additional view of the
    // very same vertex pool.
    BufferHandle meshletBuffer       = Invalid; // GPUMeshlet[]
    BufferHandle meshletVertexBuffer = Invalid; // uint32_t[]
    BufferHandle meshletTriBuffer    = Invalid; // uint8_t[] (padded to 4B)
    uint32_t     meshletCount        = 0;
};

// GPU layout structs: generated, not written. Slang owns the GPU memory
// layout; tools/zshader reflects the cooked gpu_abi module into
// <GeneratedGpuTypes.hpp> (ZHLN::GeneratedGpu), which this header includes
// and re-exports under the engine names below. A Slang edit re-emits the
// header on the next build; src/render/GpuAbi.hpp holds every struct against
// the module through the emitted AllGpuTypes inventory, and each struct
// carries the module's offsets as static_asserts.
//
// Push blocks are deliberately not here. What a pipeline pushes is the
// renderer's interface with its shaders, not something the engine publishes:
// those structs live in src/render/RenderInternal.hpp, and each is held against
// the module that reads it where it is pushed.
//
// GPUMeshlet is the one struct still written by hand: its ABI is the raw word
// protocol in instance_data.slang's fetchMeshlet (coneAxis at byte 44), which
// no std140/std430 declaration of consecutive float3s can spell (Slang seats
// it at 48), so no declaration-derived spelling of it would be the layout the
// shaders actually read. See tools/zshader/GpuTypes.cpp.

// 64-byte meshlet descriptor. basic_task / basic_mesh index it through
// a raw BDA pointer, so this layout is the authoritative GPU type.
struct alignas(16) GPUMeshlet {
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint32_t vertexCount;
    uint32_t triangleCount;

    float sphereCenter[3];
    float sphereRadius;

    float    coneApex[3];
    float    coneAxis[3];
    float    coneCutoff;
    uint32_t _pad;
};
static_assert(sizeof(GPUMeshlet) == 64);
static_assert(alignof(GPUMeshlet) == 16);

using InstanceData              = GeneratedGpu::InstanceData;
using Light                     = GeneratedGpu::Light;
using FrameUniforms             = GeneratedGpu::FrameUniforms;
using ClusterBounds             = GeneratedGpu::ClusterBounds;
using ClusterVolume             = GeneratedGpu::ClusterVolume;
using Particle                  = GeneratedGpu::Particle;
using Particle3D                = GeneratedGpu::Particle3D;
using ParticleEmitterParams     = GeneratedGpu::ParticleEmitterParams;
using MeshParticleEmitterParams = GeneratedGpu::MeshParticleEmitterParams;

struct Material {
    PipelineHandle      pipeline           = PipelineHandle::Invalid;
    PipelineHandle      prePassPipeline    = PipelineHandle::Invalid;
    ResourceGroupHandle resourceGroup      = ResourceGroupHandle::Invalid;
    BufferHandle        constantBuffer     = BufferHandle::Invalid;
    TextureHandle       albedoMap          = TextureHandle::Invalid;
    TextureHandle       normalMap          = TextureHandle::Invalid;
    TextureHandle       pbrMap             = TextureHandle::Invalid;
    TextureHandle       emissiveMap        = TextureHandle::Invalid;
    float               baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float               emissiveFactor[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
    float               metallicFactor     = 1.0f;
    float               roughnessFactor    = 1.0f;
    float               alphaCutoff        = 0.5f;
    uint32_t            alphaMode          = 0;
};

struct GlyphMetric {
    float x0, y0, x1, y1;
    float xoff, yoff, xadvance;
};

struct FontAtlas {
    TextureHandle texture = TextureHandle::Invalid;
    GlyphMetric   glyphs[96] {};
};

enum class CSGOperation : uint8_t { Difference = 0, Union = 1, Intersection = 2 };

struct CSGModifier {
    CSGOperation operation;
    std::string  operand_name;
};

template <typename T>
inline constexpr bool EnableEnumFlags = false;

template <typename T>
concept EnumFlag = std::is_enum_v<T> && EnableEnumFlags<T>;

template <EnumFlag T>
constexpr T operator|(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator&(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator^(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator~(T a) noexcept {
    return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a));
}

template <EnumFlag T>
constexpr T& operator|=(T& a, T b) noexcept {
    a = a | b;
    return a;
}

template <EnumFlag T>
constexpr T& operator&=(T& a, T b) noexcept {
    a = a & b;
    return a;
}

template <EnumFlag T>
constexpr T& operator^=(T& a, T b) noexcept {
    a = a ^ b;
    return a;
}

template <EnumFlag T>
constexpr bool operator==(T a, T b) noexcept {
    return static_cast<std::underlying_type_t<T>>(a) == static_cast<std::underlying_type_t<T>>(b);
}

template <EnumFlag T>
constexpr bool operator!=(T a, T b) noexcept {
    return !(a == b);
}

enum class DrawFlags : uint32_t {
    None            = 0,
    ExcludeFromTLAS = 1 << 0,
    Skinned         = 1 << 1,
    VisibleInMain   = 1 << 2,
    VisibleInShadow = 1 << 3,
    Hidden          = 1 << 4,
    Viewmodel       = 1 << 5,
};
} // namespace ZHLN

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::DrawFlags> = true;
