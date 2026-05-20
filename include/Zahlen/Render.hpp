#pragma once
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

class RenderContext {
  public:
	RenderContext(Window& window, const RenderConfig& cfg);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	RenderContext& operator=(const RenderContext&) = delete;

	void BeginFrame();
	void EndFrame();
	void SetResolution(const Extent2D& resolution);
	const char* GetRendererName() const;
	const char* GetGPUName() const;

	// --- Opaque Resource Creation API ---
	BufferHandle CreateVertexBuffer(const void* data, size_t size);
	BufferHandle CreateConstantBuffer(size_t size);
	Material CreateMaterial(const PipelineDesc& desc);

	uint32_t CreateTexture(const void* data, uint32_t width, uint32_t height);

	struct Impl;
	Impl* GetImpl() const { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth = 1.0f);
void SetMatrices(RenderContext& ctx, const JPH::Mat44& viewProj, const JPH::Mat44& prevViewProj);
void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform, const JPH::Mat44& prevTransform);

} // namespace Renderer

} // namespace ZHLN