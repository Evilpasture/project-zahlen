#include "engine/AssetFactory.hpp"
#include "engine/Engine.hpp"
#include "engine/Math3D.hpp"

using namespace ZHLN;

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Engine engine;

	// Focus the window (now that the engine owns it)
	engine.GetWindow().Focus();

	const Mesh tetra = AssetFactory::CreateTetrahedron(engine.GetRenderContext());
	const Material basic = AssetFactory::CreateBasicMaterial(engine.GetRenderContext());

	Physics::CreateStaticFloor(engine.GetPhysicsContext(), 50.0f);
	JPH::BodyID box =
		Physics::CreateDynamicBox(engine.GetPhysicsContext(), {0, 10, 0}, {0.5, 0.5, 0.5});

	JPH::Mat44 proj =
		Math::CreatePerspective(JPH::DegreesToRadians(45.0f), 1280.0f / 720.0f, 0.1f, 1000.0f);
	JPH::Mat44 view = Math::CreateLookAt({10, 10, 20}, {0, 2, 0}, JPH::Vec3::sAxisY());

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		engine.GetPhysicsContext().Step(1.0f / 60.0f);

		engine.BeginFrame();
		Renderer::Clear(engine.GetRenderContext(), {0.1f, 0.12f, 0.15f, 1.0f});

		JPH::Mat44 model = JPH::Mat44::sRotationTranslation(
			Physics::GetRotation(engine.GetPhysicsContext(), box),
			JPH::Vec3(Physics::GetPosition(engine.GetPhysicsContext(), box)));

		Renderer::Draw(engine.GetRenderContext(), basic, tetra, proj * view * model);

		engine.EndFrame();
	}

	return 0;
}