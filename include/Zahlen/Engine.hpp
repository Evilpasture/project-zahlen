#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Physics.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Window.hpp>
#include <memory>

namespace ZHLN {

class Engine {
  public:
	Engine();
	~Engine();

	bool IsRunning() const;
	void ProcessEvents();
	void BeginFrame();
	void EndFrame();

	RenderContext& GetRenderContext() { return *_renderContext; }
	PhysicsContext& GetPhysicsContext() { return *_physicsContext; }
	Window& GetWindow() { return *_window; }
	InputContext& GetInput() { return *_input; }
	Camera& GetCamera() { return _mainCamera; }

  private:
	std::unique_ptr<InputContext> _input;
	std::unique_ptr<Window> _window;
	std::unique_ptr<RenderContext> _renderContext;
	std::unique_ptr<PhysicsContext> _physicsContext;
	Camera _mainCamera;
};

} // namespace ZHLN