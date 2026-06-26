// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Common.h"

#include <Zahlen/Config.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <detail/String.hpp>
#include <expected>
#include <memory>
#include <optional>

namespace ZHLN {

enum class RenderFrameResult : uint8_t { Success = 0, Suboptimal, OutOfDate, DeviceLost, Error };

using RenderResult = std::expected<void, RenderFrameResult>;

struct PipelineDesc {
	const void* vertexShaderData = nullptr;
	size_t vertexShaderSize = 0;
	const void* fragShaderData = nullptr;
	size_t fragShaderSize = 0;
	bool doubleSided = false;
	bool alphaBlend = false;
	bool isLineList = false;
};

class ZHLN_API RenderContext {
  public:
	RenderContext(Window& window, const RenderConfig& cfg);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	auto operator=(const RenderContext&) -> RenderContext& = delete;

	void CheckShaderReload() noexcept;

	[[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

	[[nodiscard]] RenderResult BeginFrame() noexcept;
	[[nodiscard]] RenderResult EndFrame() noexcept;
	void SetResolution(const Extent2D& resolution);
	[[nodiscard]] const char* GetRendererName() const;
	[[nodiscard]] const char* GetGPUName() const;
	[[nodiscard]] uint32_t GetFrameIndex() const noexcept;

	// --- Opaque Resource Creation API ---
	auto CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition))
		-> BufferHandle;
	auto CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
	void DestroyBuffer(BufferHandle handle);
	auto CreateConstantBuffer(size_t size) -> BufferHandle;
	auto CreateMaterial(const PipelineDesc& desc) -> Material;

	auto CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true)
		-> uint32_t;
	auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

	void UploadDebugVertices(const void* posData, size_t posSize, const void* attrData,
							 size_t attrSize, uint32_t vertexCount) noexcept;
	[[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

	// --- NEW: Declare CreateTextureCube (Switched from std::vector to raw pointer) ---
	auto CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height)
		-> uint32_t;

	// Dynamic CPU-to-GPU Joint Matrix transfer hook
	void UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count);

	uint32_t AllocateMorphDeltas(uint32_t count, const float* deltas);

	void SetAAState(const AAState& state);

	void BuildMeshBLAS(Mesh& mesh);

	void SetShadowResolution(uint32_t resolution);

	struct Impl;
	[[nodiscard]] auto GetImpl() const -> Impl* { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

struct DrawParams {
	JPH::Mat44 transform = JPH::Mat44::sIdentity();
	JPH::Mat44 prevTransform = JPH::Mat44::sIdentity();
	float cullRadius = 1.0f;
	uint32_t jointOffset = 0;
	uint32_t morphOffset = 0;
	uint32_t activeMorphCount = 0;
	const float* morphWeights = nullptr;
	DrawFlags flags = DrawFlags::None;

	BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

	// --- Dynamic Shading Factor Overrides (-1.0f = fall back to material defaults) ---
	float roughness = -1.0f;
	float metallic = -1.0f;
};

namespace Renderer {

void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj,
				 const JPH::Mat44& unjitteredViewProj);
void SetFrameData(RenderContext& ctx, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView);
void SetGISettings(RenderContext& ctx, const GISettings& settings);

void SetLights(RenderContext& ctx, const GPULight* lights, uint32_t count);
void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const DrawParams& params);

void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex);

} // namespace Renderer

} // namespace ZHLN
