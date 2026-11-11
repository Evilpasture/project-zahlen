// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine's graphics capability. A small value-type view of the device the
// engine owns: no Vulkan headers, no RenderContext definition, no way for a
// caller to reach the renderer's internals or to keep a mutable reference to an
// engine-owned object. Engine is the only class that can construct one.
//
// Queries return values (or a const view). Mutations are operations, not
// setters on a borrowed object.

#include <Zahlen/Common.h>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/FrameScope.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <type_traits>
#include <utility>

namespace ZHLN {

class RenderContext;
struct EnvironmentRadianceDesc;

class ZHLN_API GraphicsDevice {
  public:
    // --- Device state ---------------------------------------------------------
    void SetResolution(Extent2D resolution);
    void SetViewport(const ViewportRect& rect) noexcept;
    [[nodiscard]] auto Viewport() const noexcept -> ViewportRect;
    [[nodiscard]] auto ViewportAspect() const noexcept -> float;
    [[nodiscard]] auto Info() const noexcept -> RenderInfo;
    [[nodiscard]] auto FrameIndex() const noexcept -> uint32_t;
    [[nodiscard]] auto FramebufferSize() const -> std::optional<Extent2D>;
    [[nodiscard]] auto PacedDeltaTime() const noexcept -> std::optional<float>;
    [[nodiscard]] auto PresentTiming() const noexcept -> PresentTimingMetrics;
    [[nodiscard]] auto CaptureFrame() noexcept -> std::expected<CapturedFrame, ErrorCode>;

    void               ApplySettings(const GraphicsSettings& settings) noexcept;
    [[nodiscard]] auto Settings() const noexcept -> const GraphicsSettings&;

    // Read-modify-write for callers that want to change a field or two. The
    // device stays the owner: `fn` edits a copy, and the result goes back
    // through ApplySettings, so delta detection still sees one transition.
    template <typename Fn>
        requires std::is_invocable_v<Fn, GraphicsSettings&>
    void PatchSettings(Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn, GraphicsSettings&>) {
        GraphicsSettings edited = Settings();
        std::forward<Fn>(fn)(edited);
        ApplySettings(edited);
    }
    void               SetAntiAliasing(AAState state);
    void               ClearCaches() noexcept;
    [[nodiscard]] auto SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept -> std::expected<void, ErrorCode>;

    // --- GPU resources --------------------------------------------------------
    // Value in, handle or value out. No caller-owned container reference.
    [[nodiscard]] auto CreateMaterial(const MaterialDesc& desc) -> std::expected<ZHLN::Material, ErrorCode>;
    [[nodiscard]] auto CreateBuffer(BufferDesc desc) -> BufferID;
    [[nodiscard]] auto CreateVertexBuffer(std::span<const std::byte> bytes) -> BufferID;
    [[nodiscard]] auto CreateIndexBuffer(std::span<const std::byte> bytes) -> BufferID;
    [[nodiscard]] auto CreateStorageBuffer(std::span<const std::byte> bytes) -> BufferID;
    [[nodiscard]] auto CreateStorageBufferElements(size_t count) -> BufferID;
    [[nodiscard]] auto CreateSkinScratchBuffer(size_t count) -> BufferID;
    [[nodiscard]] auto CreateMeshletBuffer(std::span<const MeshletDesc> meshlets) -> BufferID;
    [[nodiscard]] auto UploadMeshlets(const MeshletBuildResult& built) -> MeshletStreams;
    void               DestroyBuffer(BufferID handle);
    [[nodiscard]] auto UpdateBuffer(BufferID handle, std::span<const std::byte> bytes) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool isSRGB = true) -> std::expected<TextureID, ErrorCode>;
    [[nodiscard]] auto CreateTexture(std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool isSRGB = true,
                                     bool generateMips = true) -> std::expected<TextureID, ErrorCode>;
    [[nodiscard]] auto CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB = true,
                                               bool generateMips = true) -> TextureID;
    void UnloadTexture(TextureID handle);

    [[nodiscard]] auto Mesh(AssetID id) const noexcept -> std::optional<ZHLN::Mesh>;
    [[nodiscard]] auto Material(MaterialID id) const noexcept -> std::optional<ZHLN::Material>;
    void               RegisterMesh(AssetID id, ZHLN::Mesh mesh) noexcept;
    void               UnregisterMesh(AssetID id) noexcept;
    void               RegisterMaterial(MaterialID id, ZHLN::Material mat) noexcept;
    void               UnregisterMaterial(MaterialID id) noexcept;
    void               DestroyMesh(const ZHLN::Mesh& mesh) noexcept;
    [[nodiscard]] auto BuildMeshBLAS(ZHLN::Mesh& mesh) noexcept -> RenderResult;

    void UpdateJointMatrices(uint32_t offset, std::span<const ZHLN::Mat4x4> matrices);
    [[nodiscard]] auto AllocateMorphDeltas(std::span<const float> deltas) -> uint32_t;

    // --- Frame submission -----------------------------------------------------
    // Convenience overloads: typed spans are converted here, at the boundary,
    // so the caller never needs the renderer's template surface.
    template <BufferUsage Usage, typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateBuffer(std::span<T, SpanExtent> elements) -> BufferID {
        return CreateBuffer(BufferDesc {.usage = Usage, .data = std::as_bytes(elements), .stride = static_cast<uint32_t>(sizeof(T))});
    }
    template <BufferUsage Usage, typename T = std::byte>
    [[nodiscard]] auto CreateBuffer(size_t elementCount) -> BufferID {
        return CreateBuffer(BufferDesc {.usage = Usage, .size = elementCount * sizeof(T), .stride = static_cast<uint32_t>(sizeof(T))});
    }
    template <typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto UpdateBuffer(BufferID handle, std::span<T, SpanExtent> elements) noexcept -> std::expected<void, ErrorCode> {
        return UpdateBuffer(handle, std::as_bytes(elements));
    }
    template <typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateTexture(std::span<T, SpanExtent> pixels, Extent2D extent, bool isSRGB = true) -> std::expected<TextureID, ErrorCode> {
        return CreateTexture(std::as_bytes(pixels), extent, isSRGB);
    }
    template <typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateTexture(std::string_view name, std::span<T, SpanExtent> pixels, Extent2D extent, bool isSRGB = true)
        -> std::expected<TextureID, ErrorCode> {
        return CreateTexture(name, std::as_bytes(pixels), extent, isSRGB);
    }

    [[nodiscard]] auto BeginFrame() noexcept -> FrameOutcome<FrameScope>;
    void Draw(const ZHLN::Material& material, const ZHLN::Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const ZHLN::Material& material, const ZHLN::Mesh& mesh, const CSGDrawParams& params) noexcept;


  private:
    friend class Engine;
    explicit GraphicsDevice(RenderContext& ctx) noexcept: _ctx(&ctx) {
    }
    RenderContext* _ctx;
};

} // namespace ZHLN
