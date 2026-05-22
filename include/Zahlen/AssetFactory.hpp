#pragma once
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <vector>

namespace ZHLN::AssetFactory {
Mesh CreateTetrahedron(RenderContext& ctx);
Mesh CreatePlane(RenderContext& ctx, float extent = 10.0f,
				 const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents,
			   const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});
Material CreateBasicMaterial(RenderContext& ctx);
Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight,
				   std::vector<float>& outHeights);
Mesh LoadGLB(RenderContext& ctx, const std::string& path);
} // namespace ZHLN::AssetFactory
