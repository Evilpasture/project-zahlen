// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/IGraphicsDevice.hpp
//
// The renderer as the public API sees it. Every virtual is noexcept, fixed
// layout, and traffics multi-field descriptors/results (settings, buffers,
// textures, materials, meshes) through reflected ChainInNode / ChainOutNode
// lists so struct growth happens by chaining new nodes rather than freezing or
// breaking vtable slots. Inline non-virtual overloads provide one-line C++
// ergonomics in the consumer's own translation unit.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/COM/GraphicsTypes.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

#include <cstddef>
#include <concepts>
#include <span>
#include <cstdint>

namespace ZHLN::COM {

template <typename T>
concept GraphicsSettingsSlice =
    std::same_as<T, GraphicsSettings> || std::same_as<T, AAState> || std::same_as<T, GISettings> ||
    std::same_as<T, ShadowSettings> || std::same_as<T, RayTracingConfig> || std::same_as<T, EnvironmentSettings> ||
    std::same_as<T, ViewportInfo>;

struct IGraphicsDevice : IObject {
    virtual void SetResolution(Extent2D resolution) noexcept = 0;

    [[nodiscard]] virtual ViewportRect Viewport() const noexcept = 0;
    [[nodiscard]] virtual uint32_t     FrameIndex() const noexcept = 0;

    // Partial or full pipeline configuration via COM::InChain / COM::OutChain.
    // Supported nodes: GraphicsSettings, AAState, GISettings, ShadowSettings,
    // RayTracingConfig, EnvironmentSettings, ViewportInfo.
    virtual Result<void> ApplySettings(const ChainInNode* settingsChain) noexcept = 0;
    // Read-only queries additionally accept GraphicsDeviceInfo.
    virtual Result<void> GetSettings(ChainOutNode* settingsChain) const noexcept = 0;

    // Supported node: BufferCreateInfo.
    virtual Result<BufferID> CreateBuffer(const ChainInNode* bufferChain) noexcept = 0;
    virtual void                           DestroyBuffer(BufferID handle) noexcept = 0;
    virtual Result<void>     UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept = 0;

    // Supported node: TextureCreateInfo.
    virtual Result<TextureID> CreateTexture(const ChainInNode* textureChain) noexcept = 0;
    virtual void                            UnloadTexture(TextureID handle) noexcept = 0;

    // Material creation and registration.
    // CreateMaterial accepts MaterialDesc or PbrMaterialConfig in `descChain`
    // and writes ZHLN::Material into `outMaterialChain`.
    virtual Result<void> CreateMaterial(const ChainInNode* descChain,
                                                      ChainOutNode* outMaterialChain) noexcept = 0;
    virtual Result<void> RegisterMaterial(MaterialID id, const ChainInNode* materialChain) noexcept = 0;
    virtual Result<void> GetMaterial(MaterialID id, ChainOutNode* outMaterialChain) const noexcept = 0;
    virtual void                       UnregisterMaterial(MaterialID id) noexcept = 0;

    // Mesh creation, procedural generation, BLAS building and registration.
    // CreateMesh accepts exactly one geometry node (MeshStreamsDesc,
    // BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc, CylinderMeshDesc,
    // ConeMeshDesc, or TetrahedronMeshDesc) plus optional MeshBuildOptions in
    // `buildChain`, and writes ZHLN::Mesh into `outMeshChain`.
    virtual Result<void> CreateMesh(const ChainInNode* buildChain, ChainOutNode* outMeshChain) noexcept = 0;
    virtual Result<void> RegisterMesh(AssetID id, const ChainInNode* meshChain) noexcept = 0;
    virtual Result<void> GetMesh(AssetID id, ChainOutNode* outMeshChain) const noexcept = 0;
    virtual void                       UnregisterMesh(AssetID id) noexcept = 0;
    virtual Result<void> DestroyMesh(const ChainInNode* meshChain) noexcept = 0;
    virtual Result<void> BuildMeshBLAS(const ChainInNode* inMeshChain,
                                                     ChainOutNode* outMeshChain) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    template <GraphicsSettingsSlice... Slices>
        requires(sizeof...(Slices) >= 1)
    void ApplySettings(const Slices&... slices) noexcept {
        (void)ApplySettings(InChain(slices...));
    }

    [[nodiscard]] auto Info() const noexcept -> GraphicsDeviceInfo {
        GraphicsDeviceInfo info {};
        (void) GetSettings(OutChain(info));
        return info;
    }

    [[nodiscard]] auto Settings() const noexcept -> GraphicsSettings {
        GraphicsSettings settings {};
        (void)GetSettings(OutChain(settings));
        return settings;
    }

    [[nodiscard]] auto CreateBuffer(BufferUsage usage, Span<const std::byte> data, uint32_t stride = 1) noexcept
        -> BufferID {
        const Result<BufferID> created =
            CreateBuffer(InChain(BufferCreateInfo {.usage = usage, .data = data, .size = data.size, .stride = stride}));
        return created.HasValue() ? created.value : BufferID::Invalid;
    }

    auto CreateTexture(StringView name, Span<const std::byte> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        return CreateTexture(
            InChain(TextureCreateInfo {.name = name, .pixels = rgba, .extent = extent, .isSRGB = isSRGB}));
    }

    auto CreateMaterial(const MaterialDesc& desc) noexcept -> Result<ZHLN::Material> {
        ZHLN::Material     built {};
        const Result<void> outcome = CreateMaterial(InChain(desc), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    auto CreateMaterial(const PbrMaterialConfig& cfg) noexcept -> Result<ZHLN::Material> {
        ZHLN::Material     built {};
        const Result<void> outcome = CreateMaterial(InChain(cfg), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    auto CreateAndRegisterMaterial(MaterialID id, const MaterialDesc& desc) noexcept -> Result<ZHLN::Material> {
        const Result<ZHLN::Material> built = CreateMaterial(desc);
        if (!built.HasValue()) return built.error;
        RegisterMaterial(id, built.value);
        return built.value;
    }

    void RegisterMaterial(MaterialID id, const ZHLN::Material& material) noexcept {
        (void)RegisterMaterial(id, InChain(material));
    }

    [[nodiscard]] auto FindMaterial(MaterialID id) const noexcept -> Option<ZHLN::Material> {
        ZHLN::Material     found {};
        const Result<void> outcome = GetMaterial(id, OutChain(found));
        if (!outcome.HasValue()) return std::nullopt;
        return found;
    }

    auto UploadMesh(const MeshStreamsDesc& streams, bool buildBLAS = false) noexcept
        -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome =
            CreateMesh(InChain(streams, MeshBuildOptions {.buildBLAS = buildBLAS}), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateBoxMesh(ZHLN::FVec3 halfExtents, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept
        -> ZHLN::Mesh {
        ZHLN::Mesh built {};
        (void)CreateMesh(InChain(BoxMeshDesc {.halfExtents = halfExtents, .color = color}), OutChain(built));
        return built;
    }

    [[nodiscard]] auto CreatePlaneMesh(float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}) noexcept
        -> ZHLN::Mesh {
        ZHLN::Mesh built {};
        (void)CreateMesh(InChain(PlaneMeshDesc {.extent = extent, .color = color}), OutChain(built));
        return built;
    }

    [[nodiscard]] auto CreateSphereMesh(float radius = 0.5f, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept
        -> ZHLN::Mesh {
        ZHLN::Mesh built {};
        (void)CreateMesh(InChain(SphereMeshDesc {.radius = radius, .color = color}), OutChain(built));
        return built;
    }

    [[nodiscard]] auto CreateCylinderMesh(float radius = 0.5f, float height = 1.0f,
                                          ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept -> ZHLN::Mesh {
        ZHLN::Mesh built {};
        (void)CreateMesh(InChain(CylinderMeshDesc {.radius = radius, .height = height, .color = color}), OutChain(built));
        return built;
    }

    [[nodiscard]] auto CreateConeMesh(float radius = 0.5f, float height = 1.0f,
                                      ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept -> ZHLN::Mesh {
        ZHLN::Mesh built {};
        (void)CreateMesh(InChain(ConeMeshDesc {.radius = radius, .height = height, .color = color}), OutChain(built));
        return built;
    }

    void RegisterMesh(AssetID id, const ZHLN::Mesh& mesh) noexcept {
        (void)RegisterMesh(id, InChain(mesh));
    }

    [[nodiscard]] auto FindMesh(AssetID id) const noexcept -> Option<ZHLN::Mesh> {
        ZHLN::Mesh         found {};
        const Result<void> outcome = GetMesh(id, OutChain(found));
        if (!outcome.HasValue()) return std::nullopt;
        return found;
    }

    void DestroyMesh(const ZHLN::Mesh& mesh) noexcept {
        (void)DestroyMesh(InChain(mesh));
    }

    auto BuildMeshBLAS(ZHLN::Mesh& mesh) noexcept -> Result<void> {
        return BuildMeshBLAS(InChain(mesh), OutChain(mesh));
    }

    // Allocations share the renderer-owned joint palette.
    // Exhaustion fails without wrapping; allocations last for the engine lifetime.
    virtual Result<uint32_t> AllocateJoints(uint32_t count) noexcept = 0;
    virtual Result<void> UpdateJointMatrices(uint32_t offset, Span<const Mat4x4> matrices) noexcept = 0;
    virtual Result<uint32_t> AllocateMorphDeltas(Span<const float> deltas) noexcept = 0;
    // Supported chains: TextureCubeCreateInfo / RenderTextureCreateInfo.
    virtual Result<TextureID> CreateTextureCube(const ChainInNode* chain) noexcept = 0;
    virtual Result<TextureID> CreateRenderTexture(const ChainInNode* chain) noexcept = 0;
    virtual void DestroyRenderTexture(TextureID texture) noexcept = 0;
    virtual Result<void> SetEnvironmentRadiance(const ChainInNode* chain) noexcept = 0;
    // Captures the last rendered frame on every call. Metadata is CaptureInfo.
    // Returns required byte count; short buffers are untouched. Do not Tick
    // between a size query and a copy if the same snapshot is required.
    virtual Result<uint64_t> CaptureFrame(ChainOutNode* info, Span<uint8_t> pixels) noexcept = 0;

    [[nodiscard]] virtual Option<float> PacedDeltaTime() const noexcept = 0;

    [[nodiscard]] auto GetMesh(AssetID id) const noexcept -> Option<ZHLN::Mesh> {
        ZHLN::Mesh out {};
        if (!GetMesh(id, OutChain(out)).HasValue()) return std::nullopt;
        return out;
    }
    [[nodiscard]] auto GetMaterial(MaterialID id) const noexcept -> Option<ZHLN::Material> {
        ZHLN::Material out {};
        if (!GetMaterial(id, OutChain(out)).HasValue()) return std::nullopt;
        return out;
    }

    template <typename Fn>
    void PatchSettings(Fn&& fn) noexcept {
        GraphicsSettings current = Settings();
        fn(current);
        ApplySettings(current);
    }

    void SetAntiAliasing(AAMode mode) noexcept {
        ApplySettings(AAState {.mode = mode});
    }
    void SetAntiAliasing(const AAState& aa) noexcept {
        ApplySettings(aa);
    }

    auto CreateProceduralTexture(StringView name, Span<const std::byte> rgba, Extent2D extent,
                                               bool isSRGB = true) noexcept -> Result<TextureID> {
        return CreateTexture(name, rgba, extent, isSRGB);
    }
    auto CreateProceduralTexture(Span<const std::byte> rgba, Extent2D extent,
                                               bool isSRGB = true) noexcept -> Result<TextureID> {
        return CreateTexture(StringView {}, rgba, extent, isSRGB);
    }
    auto CreateTexture(std::string_view name, std::span<const uint32_t> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        const auto bytes = std::as_bytes(rgba);
        return CreateTexture(StringView(name.data(), name.size()),
                             Span<const std::byte>(bytes.data(), bytes.size()), extent, isSRGB);
    }

    void SetViewport(ZHLN::ViewportRect rect) noexcept {
        ApplySettings(ViewportInfo {.viewport = rect});
    }
    [[nodiscard]] auto CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> rgba,
                                               bool isSRGB = true) noexcept -> TextureID {
        const auto bytes   = std::as_bytes(rgba);
        const auto created = CreateTexture(StringView(name.data(), name.size()),
                                           Span<const std::byte>(bytes.data(), bytes.size()), extent, isSRGB);
        return created.HasValue() ? created.value : TextureID::Invalid;
    }
    auto CreateTexture(std::span<const uint8_t> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        const auto bytes = std::as_bytes(rgba);
        return CreateTexture(StringView {}, Span<const std::byte>(bytes.data(), bytes.size()), extent, isSRGB);
    }
    auto CreateTexture(std::span<const uint32_t> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        const auto bytes = std::as_bytes(rgba);
        return CreateTexture(StringView {}, Span<const std::byte>(bytes.data(), bytes.size()), extent, isSRGB);
    }
    auto CreateTexture(std::span<const std::byte> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        return CreateTexture(StringView {}, Span<const std::byte>(rgba.data(), rgba.size()), extent, isSRGB);
    }

protected:
    ~IGraphicsDevice() = default;
};

} // namespace ZHLN::COM
