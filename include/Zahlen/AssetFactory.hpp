#pragma once
#include "Zahlen/AssetManager.hpp"
#include "Zahlen/Entity.hpp"
#include "ecs/ECS.hpp"

#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <vector>

namespace ZHLN::AssetFactory {
Mesh CreateTetrahedron(RenderContext& ctx);
Mesh CreatePlane(RenderContext& ctx, float extent = 10.0f,
				 const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents,
			   const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});
Material CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);
Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight,
				   std::vector<float>& outHeights);
Mesh LoadGLB(RenderContext& ctx, const std::string& path);

uint32_t CreateFontAtlasTexture(RenderContext& ctx);

Mesh LoadCookedMesh(RenderContext& ctx, AssetManager& assetMgr, const std::string& virtualPath);
uint32_t LoadCookedTexture(RenderContext& ctx, AssetManager& assetMgr,
						   const std::string& virtualPath);

template <bool CreatePhysics = false>
std::vector<Entity> SpawnGLB(RenderContext& ctx, ECS::Registry& reg, const std::string& path);

// --- NEW: Expose runtime skeletal animation update loop ---
void UpdateAnimations(RenderContext& ctx, ECS::Registry& reg, float dt);

Material CreateToonMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);
} // namespace ZHLN::AssetFactory
