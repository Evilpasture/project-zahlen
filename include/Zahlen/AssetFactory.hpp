#pragma once

#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>
#include <string_view>

namespace ZHLN {
class RenderContext;
class AssetManager;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::AssetFactory {
Mesh CreateTetrahedron(RenderContext& ctx);
Mesh CreatePlane(RenderContext& ctx, float extent = 10.0f,
				 const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents,
			   const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});
Material CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);
Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight,
				   float* outHeights);
Mesh LoadGLB(RenderContext& ctx, std::string_view path);

uint32_t CreateFontAtlasTexture(RenderContext& ctx);

Mesh LoadCookedMesh(RenderContext& ctx, AssetManager& assetMgr, std::string_view virtualPath);
uint32_t LoadCookedTexture(RenderContext& ctx, AssetManager& assetMgr,
						   std::string_view virtualPath);

template <bool CreatePhysics = false>
uint32_t SpawnGLB(RenderContext& ctx, ECS::Registry& reg, std::string_view path,
				  Entity* outBuffer = nullptr, uint32_t maxCount = 0);

// --- NEW: Expose runtime skeletal animation update loop ---
void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt);

Material CreateToonMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);
} // namespace ZHLN::AssetFactory
