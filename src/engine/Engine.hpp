#pragma once
#include "engine/Physics.hpp"
#include "engine/Render.hpp"
#include "engine/Window.hpp" // New include

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

  private:
	// Order is critical: Window must exist for the RenderContext to link to it
	std::unique_ptr<Window> _window;
	std::unique_ptr<RenderContext> _renderContext;
	std::unique_ptr<PhysicsContext> _physicsContext;
};

} // namespace ZHLN