// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <engine/ModelPrefab.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <physics/Physics.hpp>
#include <Zahlen/Render/Types.hpp>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class AssetManager;
class ArticulationContext;
} // namespace ZHLN

namespace ZHLN::PrefabFactory {

// Standalone builders return caller-owned buffers: use DestroyMesh after their
// last draw. Entity spawners below attach scene-owned cleanup instead.
auto CreateTetrahedronMesh(RenderContext& gfx) -> Mesh;
auto CreatePlaneMesh(RenderContext& gfx, float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}) -> Mesh;
auto CreateBoxMesh(RenderContext& gfx, ZHLN::FVec3 halfExtents, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateSphereMesh(RenderContext& gfx, float radius, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateCylinderMesh(RenderContext& gfx, float radius, float height, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateConeMesh(RenderContext& gfx, float radius, float height, ZHLN::FVec4 color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;

// Legacy render-tool helper. Runtime hosts own atlases and upload them through
// GraphicsDevice::CreateTexture (see the host-side HostUI composition).
auto CreateFontAtlas(RenderContext& ctx) -> FontAtlas;

// Asset loading only; this does not allocate or own a runtime UI atlas.
auto LoadFontAsset(Engine& engine, std::string_view path) -> std::expected<AssetID, ErrorCode>;

auto CreateBox(Engine& engine, ZHLN::FVec3 halfExtents, const SpawnParams& params = {}) -> Entity;

auto CreatePlane(Engine& engine, float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> Entity;

auto CreateSphere(Engine& engine, float radius, const SpawnParams& params = {}) -> Entity;
auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;

auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;

} // namespace ZHLN::PrefabFactory
