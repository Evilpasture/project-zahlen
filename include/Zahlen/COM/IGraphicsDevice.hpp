// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
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

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

#include <cstddef>
#include <cstdint>

namespace ZHLN::COM {

struct IGraphicsDevice : IObject {
    virtual void SetResolution(Extent2D resolution) noexcept = 0;

    [[nodiscard]] virtual ViewportRect Viewport() const noexcept = 0;
    [[nodiscard]] virtual uint32_t     FrameIndex() const noexcept = 0;

    // Partial or full pipeline configuration via COM::InChain / COM::OutChain.
    // Supported nodes: GraphicsSettings, AAState, GISettings, ShadowSettings,
    // RayTracingConfig, EnvironmentSettings.
    [[nodiscard]] virtual Result<void> ApplySettings(const ChainInNode* settingsChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> GetSettings(ChainOutNode* settingsChain) const noexcept = 0;

    // Supported node: BufferCreateInfo.
    [[nodiscard]] virtual Result<BufferID> CreateBuffer(const ChainInNode* bufferChain) noexcept = 0;
    virtual void                           DestroyBuffer(BufferID handle) noexcept = 0;
    [[nodiscard]] virtual Result<void>     UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept = 0;

    // Supported node: TextureCreateInfo.
    [[nodiscard]] virtual Result<TextureID> CreateTexture(const ChainInNode* textureChain) noexcept = 0;
    virtual void                            UnloadTexture(TextureID handle) noexcept = 0;

    // Material creation and registration.
    // CreateMaterial accepts MaterialDesc or PbrMaterialConfig in `descChain`
    // and writes ZHLN::Material into `outMaterialChain`.
    [[nodiscard]] virtual Result<void> CreateMaterial(const ChainInNode* descChain,
                                                      ChainOutNode* outMaterialChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> RegisterMaterial(MaterialID id, const ChainInNode* materialChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> GetMaterial(MaterialID id, ChainOutNode* outMaterialChain) const noexcept = 0;
    virtual void                       UnregisterMaterial(MaterialID id) noexcept = 0;

    // Mesh creation, procedural generation, BLAS building and registration.
    // CreateMesh accepts exactly one geometry node (MeshStreamsDesc,
    // BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc, CylinderMeshDesc,
    // ConeMeshDesc, or TetrahedronMeshDesc) plus optional MeshBuildOptions in
    // `buildChain`, and writes ZHLN::Mesh into `outMeshChain`.
    [[nodiscard]] virtual Result<void> CreateMesh(const ChainInNode* buildChain, ChainOutNode* outMeshChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> RegisterMesh(AssetID id, const ChainInNode* meshChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> GetMesh(AssetID id, ChainOutNode* outMeshChain) const noexcept = 0;
    virtual void                       UnregisterMesh(AssetID id) noexcept = 0;
    [[nodiscard]] virtual Result<void> DestroyMesh(const ChainInNode* meshChain) noexcept = 0;
    [[nodiscard]] virtual Result<void> BuildMeshBLAS(const ChainInNode* inMeshChain,
                                                     ChainOutNode* outMeshChain) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    [[nodiscard]] auto ApplySettings(const GraphicsSettings& settings) noexcept -> Result<void> {
        return ApplySettings(InChain(settings));
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

    [[nodiscard]] auto CreateTexture(StringView name, Span<const std::byte> rgba, Extent2D extent,
                                     bool isSRGB = true) noexcept -> Result<TextureID> {
        return CreateTexture(
            InChain(TextureCreateInfo {.name = name, .pixels = rgba, .extent = extent, .isSRGB = isSRGB}));
    }

    [[nodiscard]] auto CreateMaterial(const MaterialDesc& desc) noexcept -> Result<ZHLN::Material> {
        ZHLN::Material     built {};
        const Result<void> outcome = CreateMaterial(InChain(desc), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateMaterial(const PbrMaterialConfig& cfg) noexcept -> Result<ZHLN::Material> {
        ZHLN::Material     built {};
        const Result<void> outcome = CreateMaterial(InChain(cfg), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateAndRegisterMaterial(MaterialID id, const MaterialDesc& desc) noexcept
        -> Result<ZHLN::Material> {
        const Result<ZHLN::Material> built = CreateMaterial(desc);
        if (!built.HasValue()) return built.error;
        const Result<void> reg = RegisterMaterial(id, InChain(built.value));
        if (!reg.HasValue()) return reg.error;
        return built.value;
    }

    [[nodiscard]] auto RegisterMaterial(MaterialID id, const ZHLN::Material& material) noexcept -> Result<void> {
        return RegisterMaterial(id, InChain(material));
    }

    [[nodiscard]] auto FindMaterial(MaterialID id) const noexcept -> Option<ZHLN::Material> {
        ZHLN::Material     found {};
        const Result<void> outcome = GetMaterial(id, OutChain(found));
        if (!outcome.HasValue()) return std::nullopt;
        return found;
    }

    [[nodiscard]] auto UploadMesh(const MeshStreamsDesc& streams, bool buildBLAS = false) noexcept
        -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome =
            CreateMesh(InChain(streams, MeshBuildOptions {.buildBLAS = buildBLAS}), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateBoxMesh(ZHLN::FVec3 halfExtents, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept
        -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome = CreateMesh(InChain(BoxMeshDesc {.halfExtents = halfExtents, .color = color}),
                                                OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreatePlaneMesh(float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}) noexcept
        -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome = CreateMesh(InChain(PlaneMeshDesc {.extent = extent, .color = color}),
                                                OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateSphereMesh(float radius = 0.5f, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept
        -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome = CreateMesh(InChain(SphereMeshDesc {.radius = radius, .color = color}),
                                                OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateCylinderMesh(float radius = 0.5f, float height = 1.0f,
                                          ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome =
            CreateMesh(InChain(CylinderMeshDesc {.radius = radius, .height = height, .color = color}), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto CreateConeMesh(float radius = 0.5f, float height = 1.0f,
                                      ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) noexcept -> Result<ZHLN::Mesh> {
        ZHLN::Mesh         built {};
        const Result<void> outcome =
            CreateMesh(InChain(ConeMeshDesc {.radius = radius, .height = height, .color = color}), OutChain(built));
        if (!outcome.HasValue()) return outcome.error;
        return built;
    }

    [[nodiscard]] auto RegisterMesh(AssetID id, const ZHLN::Mesh& mesh) noexcept -> Result<void> {
        return RegisterMesh(id, InChain(mesh));
    }

    [[nodiscard]] auto FindMesh(AssetID id) const noexcept -> Option<ZHLN::Mesh> {
        ZHLN::Mesh         found {};
        const Result<void> outcome = GetMesh(id, OutChain(found));
        if (!outcome.HasValue()) return std::nullopt;
        return found;
    }

    [[nodiscard]] auto DestroyMesh(const ZHLN::Mesh& mesh) noexcept -> Result<void> {
        return DestroyMesh(InChain(mesh));
    }

    [[nodiscard]] auto BuildMeshBLAS(ZHLN::Mesh& mesh) noexcept -> Result<void> {
        return BuildMeshBLAS(InChain(mesh), OutChain(mesh));
    }

protected:
    ~IGraphicsDevice() = default;
};

} // namespace ZHLN::COM
