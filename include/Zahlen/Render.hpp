// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Common.h"
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/render/RenderCode.hpp>
#include <expected>
#include <memory>
#include <optional>

namespace ZHLN {

namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f;
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f;
inline constexpr float FarDepth   = 1000.0f;
} // namespace Shadows

enum class RenderFrameResult : uint8_t { Success = 0, Suboptimal, OutOfDate, DeviceLost, Error };

using RenderResult = std::expected<void, Error>;

struct PipelineDesc {
    const void* vertexShaderData = nullptr;
    size_t      vertexShaderSize = 0;
    const void* fragShaderData   = nullptr;
    size_t      fragShaderSize   = 0;
    bool        doubleSided      = false;
    bool        alphaBlend       = false;
    bool        additiveBlend    = false; // ADDED: Support for emissive particles
    bool        isLineList       = false;
};

struct DrawParams {
    JPH::Mat44           transform        = JPH::Mat44::sIdentity();
    JPH::Mat44           prevTransform    = JPH::Mat44::sIdentity();
    float                cullRadius       = 1.0f;
    std::array<float, 3> localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t             jointOffset      = 0;
    uint32_t             morphOffset      = 0;
    uint32_t             activeMorphCount = 0;
    const float*         morphWeights     = nullptr;
    DrawFlags            flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    std::array<float, 4> colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f}; // alpha < 0 means disable override
    std::array<float, 4> emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f}; // alpha < 0 means disable override
};

struct CSGCutterParams {
    Mesh         mesh;
    Material     material;
    JPH::Mat44   transform           = JPH::Mat44::sIdentity();
    JPH::Mat44   prevTransform       = JPH::Mat44::sIdentity();
    float        cullRadius          = 1.0f;
    CSGOperation operation           = CSGOperation::Difference;
    uint32_t     jointOffset         = 0;
    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;
    DrawFlags    flags               = DrawFlags::None;
};

struct CSGDrawParams {
    DrawParams                   eyeParams;
    ZHLN::Array<CSGCutterParams> cutters; // Stably using your custom Array container
};

struct DecalParams {
    JPH::Mat44    transform    = JPH::Mat44::sIdentity();
    JPH::Mat44    invTransform = JPH::Mat44::sIdentity();
    TextureHandle albedoMap    = TextureHandle::Invalid;
    TextureHandle normalMap    = TextureHandle::Invalid;
    float         roughness    = 0.5f;
    float         metallic     = 0.0f;
};

struct Camera;

class ZHLN_API RenderContext {
  private:
    struct PrivateToken {
        explicit PrivateToken() = default;
    };

  public:
    struct Impl;
    RenderContext(PrivateToken, std::unique_ptr<Impl> impl) noexcept;
    ~RenderContext();

    RenderContext(const RenderContext&)                    = delete;
    auto operator=(const RenderContext&) -> RenderContext& = delete;

    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, Error> Create(Window& window, const RenderConfig& cfg) noexcept;

    void CheckShaderReload() noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

    [[nodiscard]] RenderResult BeginFrame() noexcept;
    [[nodiscard]] RenderResult EndFrame() noexcept;
    void                       SetResolution(const Extent2D& resolution);
    [[nodiscard]] const char*  GetRendererName() const;
    [[nodiscard]] const char*  GetGPUName() const;
    [[nodiscard]] uint32_t     GetFrameIndex() const noexcept;

    // --- High-Level Asset Resolution & GPU Cache API ---
    [[nodiscard]] std::optional<Mesh>     GetGPUMesh(AssetID id) const noexcept;
    [[nodiscard]] std::optional<Material> GetGPUMaterial(MaterialID id) const noexcept;
    void                                  RegisterGPUMesh(AssetID id, Mesh mesh) noexcept;
    void                                  RegisterGPUMaterial(MaterialID id, Material mat) noexcept;
    void                                  ClearGPUCaches() noexcept;

    // Reuse or create skinned scratch VBO for an entity without leaking handles
    BufferHandle GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount);
    BufferHandle CreateStorageBuffer(size_t size);
    BufferHandle GetOrCreateParticleBuffer(uint64_t entityKey, uint32_t maxParticles);
    void         SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params);
    void SubmitMeshParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const MeshParticleEmitterParams& params, AssetID mesh, MaterialID mat);

    // --- Opaque Resource Creation API ---
    auto                                         CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition)) -> BufferHandle;
    auto                                         CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
    void                                         DestroyBuffer(BufferHandle handle);
    void                                         UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept;
    auto                                         CreateConstantBuffer(size_t size) -> BufferHandle;
    [[nodiscard]] std::expected<Material, Error> CreateMaterial(const PipelineDesc& desc);
    [[nodiscard]] std::expected<Material, Error> CreateDebugLineMaterial();
    [[nodiscard]] std::expected<Material, Error> CreateDebugSolidMaterial();

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

    void SubmitUI(
        const UIBatch*          batches,
        uint32_t                batchCount,
        const VertexPosition*   positions,
        const VertexAttributes* attributes,
        uint32_t                vertexCount
    ) noexcept;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, Error>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);

    /**
     * @brief Generates a texture procedurally by invoking a CPU-side callback to populate the pixel buffer.
     * @param callback A callable with signature: void(uint32_t* pixels, uint32_t width, uint32_t height)
     */
    template <typename Func>
    [[nodiscard]] auto CreateTextureProcedural(uint32_t width, uint32_t height, bool isSRGB, Func&& callback) -> std::expected<uint32_t, Error> {
        std::vector<uint32_t> pixels(static_cast<size_t>(width * height));
        callback(pixels.data(), width, height);
        return CreateTexture(pixels.data(), width, height, isSRGB);
    }

    void     UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count);
    uint32_t AllocateMorphDeltas(uint32_t count, const float* deltas);

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked2DEmitters() noexcept;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked3DEmitters() noexcept;

    void         SetAAState(const AAState& state);
    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    [[nodiscard]] std::expected<void, Error> SetShadowResolution(uint32_t resolution);
    void                                     ProvokeDeviceLost();

    auto          BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, Error>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, Error> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    // --- OOP Idiomatic State & Command Submission APIs ---
    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;
    void SetGISettings(const GISettings& settings) noexcept;
    void SetLights(const GPULight* lights, uint32_t count) noexcept;
    void Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept;
    void DrawDecal(const DecalParams& params) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
