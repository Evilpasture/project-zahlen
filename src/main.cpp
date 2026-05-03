#include <Zahlen/AssetFactory.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <Zahlen/Platform.hpp>
#include <algorithm>


using namespace ZHLN;

// --- Logic Helpers ---

static void UpdatePlayerController(const InputContext& input, const Camera& cam,
								   PhysicsContext& ctx, EntityHandle player) {
	float yawRad = JPH::DegreesToRadians(cam.yaw);
	JPH::Vec3 forward = {std::cos(yawRad), 0.0f, std::sin(yawRad)};
	JPH::Vec3 right = {-std::sin(yawRad), 0.0f, std::cos(yawRad)};

	JPH::Vec3 move = JPH::Vec3::sZero();
	if (input.IsKeyDown(LLGL::Key::W))
		move += forward;
	if (input.IsKeyDown(LLGL::Key::S))
		move -= forward;
	if (input.IsKeyDown(LLGL::Key::A))
		move -= right;
	if (input.IsKeyDown(LLGL::Key::D))
		move += right;

	float speed = input.IsKeyDown(LLGL::Key::LShift) ? 12.0f : 5.0f;
	Physics::SetCharacterVelocity(
		ctx, player, (move.LengthSq() > 0.01f) ? move.Normalized() * speed : JPH::Vec3::sZero());
}

struct Scene {
	Mesh floor, box, player;
	Material material;
	EntityHandle playerHandle;

	void Setup(RenderContext& rc, PhysicsContext& pc) {
		floor = AssetFactory::CreatePlane(rc, 100.0f, {0.1f, 0.1f, 0.12f, 1.0f});
		box = AssetFactory::CreateBox(rc, {0.5f, 0.5f, 0.5f}, {0.8f, 0.3f, 0.2f, 1.0f});
		player = AssetFactory::CreateBox(rc, {0.5f, 0.9f, 0.5f}, {0.2f, 0.6f, 0.9f, 1.0f});
		material = AssetFactory::CreateBasicMaterial(rc);

		Physics::CreateStaticFloor(pc, 100.0f);
		for (int i = 0; i < 25; ++i) {
			Physics::CreateDynamicBox(
				pc, {(float)(i % 5) * 4.0f - 10.0f, 5.0f, (float)(i / 5) * 4.0f - 10.0f},
				{0.5f, 0.5f, 0.5f});
		}
		playerHandle = {.index = 500, .generation = 1};
		Physics::CreateCharacter(pc, {0, 2, 0}, playerHandle);
	}
};

// --- Main ---

auto main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) -> int {
	Platform::Init();
	Engine engine;
	engine.GetWindow().Focus();

	auto& rc = engine.GetRenderContext();
	auto& pc = engine.GetPhysicsContext();
	auto& input = engine.GetInput();
	auto& cam = engine.GetCamera();

	Scene scene;
	scene.Setup(rc, pc);

	const float dt = 1.0f / 60.0f;

	while (engine.IsRunning()) {
		engine.ProcessEvents();
		if (!engine.IsRunning())
			break;

		auto res = rc.GetSwapChain()->GetResolution();
		if (res.width == 0 || res.height == 0) {
			Platform::Sleep(16);
			continue;
		}
		if (input.NeedsResize()) {
			rc.SetResolution(input.GetNewSize());
			input.ClearResizeFlag();
			continue;
		}

		UpdatePlayerController(input, cam, pc, scene.playerHandle);
		if (input.IsMouseButtonDown(LLGL::Key::RButton)) {
			cam.yaw += input.GetMouse().deltaX * 0.2f;
			cam.pitch = std::clamp(cam.pitch - input.GetMouse().deltaY * 0.2f, -40.0f, 40.0f);
		}

		pc.Step(dt);

		// 1. Sync & Camera logic
		const auto& world = pc.GetWorld();
		JPH::Vec3 pPos;
		{
			std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));
			uint32_t idx = world.slotToDense[scene.playerHandle.index];
			JPH::Real* p = &world.positions[idx * 4];
			pPos = {(float)p[0], (float)p[1], (float)p[2]};
		}

		float pR = JPH::DegreesToRadians(cam.pitch), yR = JPH::DegreesToRadians(cam.yaw);
		cam.position =
			pPos + JPH::Vec3(std::cos(yR) * std::cos(pR) * -10.0f, std::sin(pR) * -10.0f + 5.0f,
							 std::sin(yR) * std::cos(pR) * -10.0f);

		// 2. Prepare Frame
		JPH::Mat44 vp =
			cam.GetProjectionMatrix((float)res.width / res.height) *
			Math::CreateLookAt(cam.position, pPos + JPH::Vec3(0, 1, 0), JPH::Vec3::sAxisY());
		Renderer::UpdateBuffer(rc, scene.material.constantBuffer.get(), vp);

		// 3. Render
		engine.BeginFrame();
		Renderer::Clear(rc, {0.08f, 0.09f, 0.12f, 1.0f});
		Renderer::Draw(rc, scene.material, scene.floor, JPH::Mat44::sIdentity());

		{
			std::lock_guard lock(const_cast<Mutex&>(world.shadowLock));
			for (size_t i = 1; i < world.count.load(std::memory_order_acquire); ++i) {
				JPH::Real* p = &world.positions[i * 4];
				float* r = &world.rotations[i * 4];
				JPH::Mat44 model = Math::CreateTransform({(float)p[0], (float)p[1], (float)p[2]},
														 {r[0], r[1], r[2], r[3]});
				Renderer::Draw(
					rc, scene.material,
					(world.denseToSlot[i] == scene.playerHandle.index ? scene.player : scene.box),
					model);
			}
		}
		engine.EndFrame();
	}
	return 0;
}