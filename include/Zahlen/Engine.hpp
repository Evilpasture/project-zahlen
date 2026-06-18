// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Engine.hpp
#pragma once
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Array.h>
// clang-format on

#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Entity.hpp>
#include <memory>

namespace ZHLN {

class InputContext;
class Window;
class RenderContext;
class PhysicsContext;
class AudioContext;
class AssetManager;
struct Camera;
struct EngineImpl;

namespace ALife {
class Simulator;
}

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
} // namespace ECS

class CullingSystem;

class ZHLN_API Engine {
  public:
	Engine();
	Engine(const EngineConfig& cfg);
	Engine(const EngineConfig& cfg, bool& outSuccess);
	~Engine();

	void HandleDeviceLost() noexcept;

	// Static Factory: Uses a raw out-pointer to avoid <string>/<expected> dependencies
	static std::unique_ptr<Engine> Create(const EngineConfig& cfg, const char** outError = nullptr);

	[[nodiscard]] bool IsRunning() const;
	void ProcessEvents();
	[[nodiscard]] bool BeginFrame(bool& outDeviceLost) noexcept;
	[[nodiscard]] bool EndFrame(bool& outDeviceLost) noexcept;

	Window& GetWindow();
	PhysicsContext& GetPhysicsContext();
	RenderContext& GetRenderContext();
	InputContext& GetInput();
	Camera& GetCamera();
	ALife::Simulator& GetALife();
	AssetManager& GetAssetManager();
	AudioContext& GetAudioContext();
	ECS::Registry& GetRegistry();

	ECS::SystemGraph& GetUpdateGraph();
	ECS::SystemGraph& GetRenderGraph();
	ECS::EntityCommandBuffer& GetMainECB();
	CullingSystem& GetCullingSystem();
	JPH::Array<Entity>& GetVisibleEntities();
	float& GetCurrentAlpha();

	[[nodiscard]] void* GetGameState() const;
	void SetGameState(void* state);
	[[nodiscard]] uint64_t GetCurrentFrame() const noexcept;

  private:
	void InitInternal(const EngineConfig& cfg, bool& outSuccess, const char** outError = nullptr);
	std::unique_ptr<EngineImpl> _impl;
};

Engine* GetEngineContext();
} // namespace ZHLN
