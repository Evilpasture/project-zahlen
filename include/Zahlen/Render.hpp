#pragma once
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/detail/String.hpp>
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
	RenderContext(Window& window);
	~RenderContext();

	RenderContext(const RenderContext&) = delete;
	RenderContext& operator=(const RenderContext&) = delete;

	void BeginFrame();
	void EndFrame();
	void SetResolution(const Extent2D& resolution);
	const char* GetRendererName() const;

	// --- Opaque Resource Creation API ---
	BufferHandle CreateVertexBuffer(const void* data, size_t size);
	BufferHandle CreateConstantBuffer(size_t size);
	Material CreateMaterial(const PipelineDesc& desc);

	struct Impl;
	Impl* GetImpl() const { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Renderer {
void Clear(RenderContext& ctx, const JPH::Vec4& color, float depth = 1.0f);
void UpdateBuffer(RenderContext& ctx, BufferHandle buffer, const void* data, size_t size);

template <typename T>
inline void UpdateBuffer(RenderContext& ctx, BufferHandle buffer, const T& data) {
	UpdateBuffer(ctx, buffer, static_cast<const void*>(&data), sizeof(T));
}

void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh,
		  const JPH::Mat44& transform);
} // namespace Renderer

} // namespace ZHLN