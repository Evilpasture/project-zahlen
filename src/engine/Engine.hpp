#pragma once
#include "engine/Render.hpp"
#include "engine/Types.hpp"

#include <functional>
#include <vector>

namespace ZHLN {

class Engine {
  public:
	Engine();
	~Engine();

	bool IsRunning() const;
	void ProcessEvents();

	void BeginFrame();
	void EndFrame();

	Mesh CreateTetrahedron();
	Material CreateMaterial();

	RenderContext& GetContext() { return *_context; }

  private:
	std::unique_ptr<RenderContext> _context;

	// A simple garbage collector: when the engine dies, it calls these to free GPU memory
	std::vector<std::function<void()>> _cleanupQueue;
};

} // namespace ZHLN