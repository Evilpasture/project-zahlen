// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <engine/ModelPrefab.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Error.hpp>
#include <expected>
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

// Entity spawners attach scene-owned cleanup; a mesh built through the COM
// device is yours to DestroyMesh after its last draw.

auto CreateBox(Engine& engine, ZHLN::FVec3 halfExtents, const SpawnParams& params = {}) -> std::expected<Entity, ErrorCode>;

auto CreatePlane(Engine& engine, float extent = 10.0f, ZHLN::FVec4 color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> std::expected<Entity, ErrorCode>;

auto CreateSphere(Engine& engine, float radius, const SpawnParams& params = {}) -> std::expected<Entity, ErrorCode>;
auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> std::expected<Entity, ErrorCode>;
auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> std::expected<Entity, ErrorCode>;

auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&>;

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;

} // namespace ZHLN::PrefabFactory
