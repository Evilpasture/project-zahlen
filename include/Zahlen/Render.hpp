#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <detail/String.hpp>
#include <memory>

namespace ZHLN {

struct PipelineDesc {
	const void* vertexShaderData = nullptr;
	size_t vertexShaderSize = 0;
	const void* fragShaderData = nullptr;
	size_t fragShaderSize = 0;
};

class ZHLN_API RenderContext {
  public:
	RenderContext(Window& window, const RenderConfig& cfg);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	auto operator=(const RenderContext&) -> RenderContext& = delete;

	void BeginFrame();
	void EndFrame();
	void SetResolution(const Extent2D& resolution);
	[[nodiscard]] const char* GetRendererName() const;
	[[nodiscard]] const char* GetGPUName() const;

	// --- Opaque Resource Creation API ---
	auto CreateVertexBuffer(const void* data, size_t size) -> BufferHandle;
	auto CreateConstantBuffer(size_t size) -> BufferHandle;
	auto CreateMaterial(const PipelineDesc& desc) -> Material;

	auto CreateTexture(const void* data, uint32_t width, uint32_t height) -> uint32_t;

	struct Impl;
	[[nodiscard]] auto GetImpl() const -> Impl* { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth = 1.0f,
		   bool useSecondaries = true);
void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj, const JPH::Mat44& prevViewProj);
void SetFrameData(RenderContext& ctx, const FrameUniforms& uniforms,
				  const JPH::Mat44& shadowProjView);
void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform, float cullRadius = 1.0f);
void DrawUI(RenderContext& ctx, const Mesh& mesh, uint32_t fontIndex);

} // namespace Renderer

} // namespace ZHLN
