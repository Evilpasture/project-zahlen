#pragma once
#include "engine/Render.hpp"
#include "engine/Types.hpp"

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
};

} // namespace ZHLN