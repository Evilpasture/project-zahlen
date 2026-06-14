// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Engine.hpp
#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
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
}

class ZHLN_API Engine {
  public:
	Engine();
	Engine(const EngineConfig& cfg);
	Engine(const EngineConfig& cfg, bool& outSuccess);
	~Engine();

	// Static Factory: Uses a raw out-pointer to avoid <string>/<expected> dependencies
	static std::unique_ptr<Engine> Create(const EngineConfig& cfg, const char** outError = nullptr);

	[[nodiscard]] bool IsRunning() const;
	void ProcessEvents();
	void BeginFrame();
	void EndFrame();

	Window& GetWindow();
	PhysicsContext& GetPhysicsContext();
	RenderContext& GetRenderContext();
	InputContext& GetInput();
	Camera& GetCamera();
	ALife::Simulator& GetALife();
	AssetManager& GetAssetManager();
	AudioContext& GetAudioContext();
	ECS::Registry& GetRegistry();

	[[nodiscard]] void* GetGameState() const;
	void SetGameState(void* state);

  private:
	void InitInternal(const EngineConfig& cfg, bool& outSuccess);
	std::unique_ptr<EngineImpl> _impl;
};

Engine* GetEngineContext();
} // namespace ZHLN
