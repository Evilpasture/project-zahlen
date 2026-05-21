#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/alife/Simulator.hpp>
#include <ecs/ECS.hpp>
#include <memory>
#include <physics/Physics.hpp>

namespace ZHLN {

class Engine {
  public:
	Engine();
	~Engine();

	bool IsRunning() const;
	void ProcessEvents();
	void BeginFrame();
	void EndFrame();

	Engine(const EngineConfig& cfg = EngineConfig{});

	RenderContext& GetRenderContext() { return *_renderContext; }
	PhysicsContext& GetPhysicsContext() { return *_physicsContext; }
	Window& GetWindow() { return *_window; }
	InputContext& GetInput() { return *_input; }
	Camera& GetCamera() { return _mainCamera; }
	ALife::Simulator& GetALife() { return *_alifeSimulator; }

	ECS::Registry& GetRegistry() { return _registry; }

  private:
	std::unique_ptr<InputContext> _input;
	std::unique_ptr<Window> _window;
	std::unique_ptr<RenderContext> _renderContext;
	std::unique_ptr<PhysicsContext> _physicsContext;
	std::unique_ptr<ALife::Simulator> _alifeSimulator;
	Camera _mainCamera;
	ECS::Registry _registry;
};
Engine* GetEngineContext();
} // namespace ZHLN