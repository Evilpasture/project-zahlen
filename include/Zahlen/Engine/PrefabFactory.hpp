// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Render/Types.hpp>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class AssetManager;
class ArticulationSystem;
} // namespace ZHLN

namespace ZHLN::PrefabFactory {

// Standalone builders return caller-owned buffers: use DestroyMesh after their
// last draw. Entity spawners below attach scene-owned cleanup instead.
ZHLN_API auto CreateTetrahedronMesh(GraphicsDevice gfx) -> Mesh;
ZHLN_API auto CreatePlaneMesh(GraphicsDevice gfx, float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}) -> Mesh;
ZHLN_API auto CreateBoxMesh(GraphicsDevice gfx, ZHLN::FVec3 halfExtents, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
ZHLN_API auto CreateSphereMesh(GraphicsDevice gfx, float radius, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
ZHLN_API auto CreateCylinderMesh(GraphicsDevice gfx, float radius, float height, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
ZHLN_API auto CreateConeMesh(GraphicsDevice gfx, float radius, float height, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;

ZHLN_API auto CreateFontAtlas(RenderContext& ctx) -> FontAtlas;
ZHLN_API auto CreateFontAtlas(RenderContext& ctx, ZHLN::Optional<AssetManager&> assetMgr, AssetID fontID) -> FontAtlas;
ZHLN_API auto CreateFontAtlas(RenderContext& ctx, AssetManager& assetMgr, std::string_view path) -> FontAtlas;

ZHLN_API auto PrimeDefaultBakedFont(AssetManager& assetMgr) -> bool;

ZHLN_API auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode>;
ZHLN_API auto LoadFontAsset(Engine& engine, std::string_view path) -> std::expected<AssetID, ErrorCode>;

ZHLN_API auto GetFontAsset(AssetManager& assetMgr, AssetID id) -> ZHLN::Optional<GUI::BakedFontAsset&>;
ZHLN_API auto GetFontAsset(AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<GUI::BakedFontAsset&>;

ZHLN_API auto LoadTexture(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, bool isSRGB = true) -> TextureID;

ZHLN_API auto CreateBox(Engine& engine, ZHLN::FVec3 halfExtents, const SpawnParams& params = {}) -> Entity;

ZHLN_API auto CreatePlane(Engine& engine, float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> Entity;

ZHLN_API auto CreateSphere(Engine& engine, float radius, const SpawnParams& params = {}) -> Entity;
ZHLN_API auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;
ZHLN_API auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;

auto LoadModelPrefab(GraphicsDevice gfx, AssetManager& assetMgr, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;
auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;

ZHLN_API auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
ZHLN_API auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;

} // namespace ZHLN::PrefabFactory
