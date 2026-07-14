// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Common.h"
#include <Zahlen/Config.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/render/RenderCode.hpp>
#include <detail/String.hpp>
#include <expected>
#include <memory>
#include <optional>

namespace ZHLN {

// --- CENTRALIZED SHADOW PROJECTION CONSTANTS ---
namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f; // For Cascades 0-2 (millimeter-precise)
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f; // For Cascade 3 (prevents distant fog clipping)
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
    bool        isLineList       = false;
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

    // --- Opaque Resource Creation API ---
    auto                                         CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition)) -> BufferHandle;
    auto                                         CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
    void                                         DestroyBuffer(BufferHandle handle);
    auto                                         CreateConstantBuffer(size_t size) -> BufferHandle;
    [[nodiscard]] std::expected<Material, Error> CreateMaterial(const PipelineDesc& desc);

    auto CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> uint32_t;
    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

    // --- NEW: Declare CreateTextureCube (Switched from std::vector to raw pointer) ---
    auto CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> uint32_t;

    // Dynamic CPU-to-GPU Joint Matrix transfer hook
    void UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count);

    uint32_t AllocateMorphDeltas(uint32_t count, const float* deltas);

    void SetAAState(const AAState& state);

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    [[nodiscard]] std::expected<void, Error> SetShadowResolution(uint32_t resolution);

    void ProvokeDeviceLost();

    /**
     * @brief Dispatches a GPU compute pass to bake a procedural noise texture
     * on-the-fly and registers it in the bindless texture array.
     * @return The bindless texture index.
     */
    auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, Error>;

    [[nodiscard]] auto GetImpl() const -> Impl* {
        return _impl.get();
    }

  private:
    std::unique_ptr<Impl> _impl;
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

    // --- Dynamic Shading Factor Overrides (-1.0f = fall back to material defaults) ---
    float roughness = -1.0f;
    float metallic  = -1.0f;
};

namespace Renderer {

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj);
void SetFrameData(RenderContext& ctx, const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView);
void SetGISettings(RenderContext& ctx, const GISettings& settings);

void SetLights(RenderContext& ctx, const GPULight* lights, uint32_t count);
void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const DrawParams& params);

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex, bool useScissor = false, ScissorRect scissorRect = {});

} // namespace Renderer

} // namespace ZHLN
