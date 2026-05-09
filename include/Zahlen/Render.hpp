#pragma once
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <memory>

namespace ZHLN {

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

	// --- Scene Setup API ---
	void SetCamera(const JPH::Mat44& viewProj, const JPH::Vec3& cameraPosition);
	void SetSunlight(const JPH::Vec3& direction, const JPH::Vec3& color, float intensity);

	// --- Resource Creation API ---
	Mesh CreateMesh(const Vertex* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount);
	
	// Uploads an image and returns its Bindless Registry Index
	uint32_t CreateTexture(const void* pixels, uint32_t width, uint32_t height);
	
	// Creates a default material pointing to fallback textures
	Material CreateMaterial();

	struct Impl;
	Impl* GetImpl() const { return _impl.get(); }

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Renderer {
	// Queues a mesh to be drawn in the PBR Render Graph
	void Draw(RenderContext& ctx, const Material& material, const Mesh& mesh, const JPH::Mat44& transform);
}

} // namespace ZHLN