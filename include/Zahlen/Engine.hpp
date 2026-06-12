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
	~Engine();

	[[nodiscard]] bool IsRunning() const;
	void ProcessEvents();
	void BeginFrame();
	void EndFrame();

	// FACADE ACCESSORS: Zero transitive header footprint
	Window& GetWindow();
	PhysicsContext& GetPhysicsContext();
	RenderContext& GetRenderContext();
	InputContext& GetInput();
	Camera& GetCamera();
	ALife::Simulator& GetALife();
	AssetManager& GetAssetManager();
	AudioContext& GetAudioContext();
	ECS::Registry& GetRegistry();

	// Single instance-local context slot for flat shared memory
	[[nodiscard]] void* GetGameState() const;
	void SetGameState(void* state);

  private:
	std::unique_ptr<EngineImpl> _impl;
};

Engine* GetEngineContext();
} // namespace ZHLN
