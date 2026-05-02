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
	// Order is critical: Window must exist for the RenderContext to link to it
	std::unique_ptr<Window> _window;
	std::unique_ptr<RenderContext> _renderContext;
	std::unique_ptr<PhysicsContext> _physicsContext;
	std::shared_ptr<InputContext> _input;
	Camera _mainCamera;
};

} // namespace ZHLN