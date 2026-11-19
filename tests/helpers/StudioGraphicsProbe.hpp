// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/IGraphicsDevice.hpp>
namespace ZHLN::COM {
// Stack-owned, borrowed COM decorator for resource-lifetime regression tests.
// It never queries or unwraps a native Engine/renderer.
struct StudioGraphicsProbe final : IGraphicsDevice {
    IGraphicsDevice& next;
    bool failMaterial = false;
    uint32_t createdMeshes = 0, destroyedMeshes = 0, createdTextures = 0, unloadedTextures = 0;
    explicit StudioGraphicsProbe(IGraphicsDevice& device): next(device) {}
    uint32_t AddRef() noexcept override { return 1; }
    uint32_t Release() noexcept override { return 1; }
    void* QueryRaw(InterfaceID) noexcept override { return nullptr; }
    void SetResolution(Extent2D resolution) noexcept override { next.SetResolution(resolution); }
    ViewportRect Viewport() const noexcept override { return next.Viewport(); }
    uint32_t FrameIndex() const noexcept override { return next.FrameIndex(); }
    Result<void> ApplySettings(const ChainInNode* settingsChain) noexcept override { return next.ApplySettings(settingsChain); }
    Result<void> GetSettings(ChainOutNode* settingsChain) const noexcept override { return next.GetSettings(settingsChain); }
    Result<BufferID> CreateBuffer(const ChainInNode* bufferChain) noexcept override { return next.CreateBuffer(bufferChain); }
    void DestroyBuffer(BufferID handle) noexcept override { next.DestroyBuffer(handle); }
    Result<void> UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept override { return next.UpdateBuffer(handle, data); }
    Result<TextureID> CreateTexture(const ChainInNode* textureChain) noexcept override { const auto result = next.CreateTexture(textureChain); if (result.HasValue()) ++createdTextures; return result; }
    void UnloadTexture(TextureID handle) noexcept override { ++unloadedTextures; next.UnloadTexture(handle); }
    Result<void> CreateMaterial(const ChainInNode* descChain, ChainOutNode* outMaterialChain) noexcept override { if (failMaterial) return Error::InvalidArgument; return next.CreateMaterial(descChain, outMaterialChain); }
    Result<void> RegisterMaterial(MaterialID id, const ChainInNode* materialChain) noexcept override { return next.RegisterMaterial(id, materialChain); }
    Result<void> GetMaterial(MaterialID id, ChainOutNode* outMaterialChain) const noexcept override { return next.GetMaterial(id, outMaterialChain); }
    void UnregisterMaterial(MaterialID id) noexcept override { next.UnregisterMaterial(id); }
    Result<void> CreateMesh(const ChainInNode* buildChain, ChainOutNode* outMeshChain) noexcept override { const auto result = next.CreateMesh(buildChain, outMeshChain); if (result.HasValue()) ++createdMeshes; return result; }
    Result<void> RegisterMesh(AssetID id, const ChainInNode* meshChain) noexcept override { return next.RegisterMesh(id, meshChain); }
    Result<void> GetMesh(AssetID id, ChainOutNode* outMeshChain) const noexcept override { return next.GetMesh(id, outMeshChain); }
    void UnregisterMesh(AssetID id) noexcept override { next.UnregisterMesh(id); }
    Result<void> DestroyMesh(const ChainInNode* meshChain) noexcept override { ++destroyedMeshes; return next.DestroyMesh(meshChain); }
    Result<void> BuildMeshBLAS(const ChainInNode* inMeshChain, ChainOutNode* outMeshChain) noexcept override { return next.BuildMeshBLAS(inMeshChain, outMeshChain); }
    Result<uint32_t> AllocateJoints(uint32_t count) noexcept override { return next.AllocateJoints(count); }
    Result<void> UpdateJointMatrices(uint32_t offset, Span<const Mat4x4> matrices) noexcept override { return next.UpdateJointMatrices(offset, matrices); }
    Result<uint32_t> AllocateMorphDeltas(Span<const float> deltas) noexcept override { return next.AllocateMorphDeltas(deltas); }
    Result<TextureID> CreateTextureCube(const ChainInNode* chain) noexcept override { return next.CreateTextureCube(chain); }
    Result<TextureID> CreateRenderTexture(const ChainInNode* chain) noexcept override { return next.CreateRenderTexture(chain); }
    void DestroyRenderTexture(TextureID texture) noexcept override { next.DestroyRenderTexture(texture); }
    Result<void> SetEnvironmentRadiance(const ChainInNode* chain) noexcept override { return next.SetEnvironmentRadiance(chain); }
    Result<uint64_t> CaptureFrame(ChainOutNode* info, Span<uint8_t> pixels) noexcept override { return next.CaptureFrame(info, pixels); }
    Option<float> PacedDeltaTime() const noexcept override { return next.PacedDeltaTime(); }
};
} // namespace ZHLN::COM
