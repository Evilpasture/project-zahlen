#include "engine/Engine.hpp"

using namespace ZHLN;

auto main([[maybe_unused]] const int argc, [[maybe_unused]] const char* const argv[]) -> int {
	Engine engine;

	// Abstracted Asset Loading
	const Mesh tetrahedron = engine.CreateTetrahedron();
	const Material flatColor = engine.CreateMaterial();

	const JPH::Vec4 background(0.12f, 0.14f, 0.16f, 1.0f);
	float rotation = 0.0f;

	// View Matrices
	JPH::Mat44 proj =
		JPH::Mat44::sPerspective(JPH::DegreesToRadians(45.0f), 1280.0f / 720.0f, 0.1f, 100.0f);
	JPH::Mat44 view = JPH::Mat44::sTranslation({0.0f, -0.2f, -3.0f});

	while (engine.IsRunning()) {
		engine.ProcessEvents();

		// Game Logic / Simulation
		rotation += 0.02f;
		JPH::Mat44 model =
			JPH::Mat44::sRotationY(rotation) * JPH::Mat44::sRotationX(rotation * 0.5f);
		JPH::Mat44 transform = proj * view * model;

		// Render Pass
		engine.BeginFrame();

		Renderer::Clear(engine.GetContext(), background);
		Renderer::Draw(engine.GetContext(), flatColor, tetrahedron, transform);

		engine.EndFrame();
	}

	return 0;
}