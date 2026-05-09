#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <Zahlen/Platform.hpp>

using namespace ZHLN;

auto main() -> int {
	Platform::Init();
	Engine engine;
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();

	Mesh floor = AssetFactory::CreatePlane(rc, 100.0f, {1, 1, 1, 1});
	Mesh box = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {1, 1, 1, 1});
	Material material = AssetFactory::CreateBasicMaterial(rc);

	Physics::CreateStaticFloor(pc, 100.0f);
	for (int i = 0; i < 25; ++i) {
		Physics::CreateDynamicBox(pc, {(float)(i % 5) * 4.0f - 10.0f, 5.0f, (float)(i / 5.0f) * 4.0f - 10.0f}, {0.5f, 0.5f, 0.5f});
	}
	EntityHandle player = Physics::CreateCharacter(pc, {0, 2, 0}, {.index = 500, .generation = 1});

	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning()) break;
		if (engine.GetWindow().GetSize().width == 0) { Platform::Sleep(16); continue; }

		// Simple Physics Control
		JPH::Vec3 move = JPH::Vec3::sZero();
		if (input.IsKeyDown(KeyCode::W)) move += {0, 0, 1};
		Physics::SetCharacterVelocity(pc, player, move * 5.0f);
		pc.Step(dt);

		// Upload correct Camera matrices
		auto res = engine.GetWindow().GetSize();
		JPH::Mat44 vp = cam.GetProjectionMatrix((float)res.width / res.height) * cam.GetViewMatrix();
		rc.SetCamera(vp, cam.position);
		rc.SetSunlight({-0.5f, -1.0f, 0.5f}, {1.0f, 0.9f, 0.8f}, 5.0f);

		// Render Frame
		engine.BeginFrame();
		
		Renderer::Draw(rc, material, floor, JPH::Mat44::sTranslation({0, -1, 0}));
		
		const auto& world = pc.GetWorld();
		std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));
		for (size_t i = 1; i < world.count.load(); ++i) {
			JPH::Real* p = &world.positions[i * 4];
			float* r = &world.rotations[i * 4];
			JPH::Mat44 model = Math::CreateTransform({(float)p[0], (float)p[1], (float)p[2]}, {r[0], r[1], r[2], r[3]});
			Renderer::Draw(rc, material, box, model);
		}

		engine.EndFrame(); // Executes the entire Render Graph!
	}
	return 0;
}