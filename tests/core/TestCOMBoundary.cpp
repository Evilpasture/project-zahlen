// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestCOMBoundary.cpp
//
// The public API as a consumer sees it. Apart from the test framework this
// file includes only Zahlen/COM, Zahlen/Core/COM and pure value headers -- no
// engine header, no PIMPL, no internal access -- and every engine call it
// makes is a virtual dispatch through a vtable into libzahlen_engine.so.
// EngineCOM's definition is not visible here, so nothing can be inlined or
// devirtualised away.

#include "TestsFramework.hpp"
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/RenderIDs.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace COMNameTest {
struct Outer { struct Value {}; };
namespace Other { struct Value {}; }
template <typename T, int N> struct Component { T value[N > 0 ? N : 1]; };
template <typename T> struct OuterTemplate { struct Value {}; };
template <auto V> struct Value {};
enum class Sign : int { Negative = -3 };
template <template <typename> class T> struct TemplateArg {};
template <typename T> struct TypeArg {};
} // namespace COMNameTest

static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::Value<-3>>() == "COMNameTest::Value<V{int:-3}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::Value<COMNameTest::Sign::Negative>>() == "COMNameTest::Value<V{COMNameTest::Sign:-3}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::Value<true>>() != ZHLN::Reflect::CanonicalTypeName<COMNameTest::Value<1>>());
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::TypeArg<const COMNameTest::Outer*>>() == "COMNameTest::TypeArg<T{P{C{COMNameTest::Outer}}}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::TypeArg<int[3]>>() == "COMNameTest::TypeArg<T{A{3:int}}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::TemplateArg<COMNameTest::TypeArg>>() == "COMNameTest::TemplateArg<M{COMNameTest::TypeArg}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::Component<int, 2>>() == "COMNameTest::Component<T{int}V{int:2}>");
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::Component<int, 2>>() != ZHLN::Reflect::CanonicalTypeName<COMNameTest::Component<int, 3>>());
static_assert(ZHLN::Reflect::CanonicalTypeName<COMNameTest::OuterTemplate<int>::Value>() != ZHLN::Reflect::CanonicalTypeName<COMNameTest::OuterTemplate<float>::Value>());
static_assert(std::string_view(ZHLN::COM::IEntities::ComponentName<COMNameTest::Component<int, 2>>()) == ZHLN::Reflect::CanonicalTypeName<COMNameTest::Component<int, 2>>());
static_assert(std::string_view(ZHLN::COM::IEntities::ComponentName<COMNameTest::Outer::Value>()) == "COMNameTest::Outer::Value");
static_assert(std::string_view(ZHLN::COM::IEntities::ComponentName<COMNameTest::Other::Value>()) == "COMNameTest::Other::Value");

namespace COMIdentityTest {

// A component the library has never heard of, declared in the consumer's TU.
struct ClientTransform {
    float position[3];
    float scale;
};

struct UnknownEngineSlice {
    uint64_t magic = 0xDEADBEEFULL;
};

} // namespace COMIdentityTest

namespace {
using COMIdentityTest::ClientTransform;

enum class BoundaryTestError : uint8_t { QueryFailed = 1 };

// The whole surface is reachable without any engine header.
static_assert(ZHLN::COM::ComComponent<ClientTransform>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Option<ZHLN::DVec3>>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::Option<ZHLN::DVec3>>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Option<ZHLN::Physics::BodyStateSnapshot>>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::Option<ZHLN::Physics::BodyStateSnapshot>>);

// Every public COM interface satisfies ComInterfaceLayoutCompliant, and
// EngineFactory's config slices cross as non-intrusive ChainInNode lists.
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IEngine>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IGraphicsDevice>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IEntities>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::ISimulation>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IAudioDevice>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IAssetLibrary>());
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IActionContext>());
static_assert(std::is_abstract_v<ZHLN::COM::IEngine>);
static_assert(std::is_abstract_v<ZHLN::COM::IGraphicsDevice>);
static_assert(std::is_abstract_v<ZHLN::COM::IEntities>);
static_assert(std::is_abstract_v<ZHLN::COM::ISimulation>);
static_assert(std::is_abstract_v<ZHLN::COM::IAudioDevice>);
static_assert(std::is_abstract_v<ZHLN::COM::IAssetLibrary>);
static_assert(std::is_abstract_v<ZHLN::COM::IActionContext>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IEngine>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IGraphicsDevice>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IEntities>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::ISimulation>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IAudioDevice>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IAssetLibrary>);
static_assert(!std::has_virtual_destructor_v<ZHLN::COM::IActionContext>);

// Distinct identities, and stable across translation units.
// The audit inspects the vtable, and only the vtable.
namespace AuditProbe {

struct ContainerOnTheWire {
    virtual auto Bodies() const noexcept -> std::vector<uint32_t> = 0;

protected:
    ~ContainerOnTheWire() = default;
};

static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<ContainerOnTheWire>());

struct ContainerInTheHeader {
    [[nodiscard]] virtual auto Bodies(uint32_t* out, uint32_t capacity) const noexcept -> uint32_t = 0;

    [[nodiscard]] auto Bodies() const -> std::vector<uint32_t> {
        std::vector<uint32_t> bodies(Bodies(nullptr, 0));
        bodies.resize(Bodies(bodies.data(), static_cast<uint32_t>(bodies.size())));
        return bodies;
    }

protected:
    ~ContainerInTheHeader() = default;
};

static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ContainerInTheHeader>());

struct VirtualDestructor {
    virtual ~VirtualDestructor() = default;
};

static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<VirtualDestructor>());

} // namespace AuditProbe

static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEngine>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IGraphicsDevice>());
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEngine>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEntities>());
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::ISimulation>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IAudioDevice>());
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::IAssetLibrary>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IActionContext>());
static_assert(ZHLN::COM::InterfaceIdOf<ZHLN::COM::ISimulation>() != ZHLN::COM::InterfaceIdOf<ZHLN::COM::IEngine>());

static_assert(std::is_trivially_copyable_v<ZHLN::Physics::RaycastResult>);
static_assert(std::is_trivially_copyable_v<ZHLN::VoiceDesc>);
static_assert(std::is_trivially_copyable_v<ZHLN::AudioEvent>);
static_assert(std::is_trivially_copyable_v<ZHLN::ActionState>);
static_assert(std::is_trivially_copyable_v<ZHLN::PrefabFactory::SpawnParams>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::PrefabInfo>);
static_assert(ZHLN::COM::IEntities::ComponentName<ClientTransform>().size > 0);

// A consumer's own declaration of engine components. Nothing here includes
// Components.hpp: the layout is restated, and the engine's recursive leaf
// LayoutHash check decides whether the restatement agrees with the library.
namespace ConsumerSide {

struct alignas(16) Mat44Bytes {
    float m[16];
};

struct WorldTransformComponent {
    Mat44Bytes world;
    Mat44Bytes previous;
};

struct PBRComponent {
    float roughness = 0.5F;
    float metallic  = 0.0F;
};

struct PhysicsComponent {
    ZHLN::Physics::BodyID physicsID {};
    bool                  isStatic = false;
};

// Deliberately wrong: one extra field, so size and hash both diverge from the
// engine's MeshComponent while the name still resolves to a published family.
struct MeshComponent {
    uint32_t meshAsset;
    uint32_t materialAsset;
    float    cullRadius;
    float    localCenter[3];
    uint32_t flags;
    int32_t  nodeIndex;
    int32_t  bogusExtraField;
};

} // namespace ConsumerSide

void CheckGraphics(ZHLN::COM::IGraphicsDevice& graphics, ZHLN::COM::IAssetLibrary& assets) {
    // Sub-slice chain mutation + sub-slice chain query.
    const ZHLN::ShadowSettings shadowIn {.width = 140.0F, .resolution = 1024, .sunSize = 0.08F};
    const ZHLN::AAState        aaIn {.mode = ZHLN::AAMode::FXAA, .fxaaSubpix = 0.5F};
    ZHLN::Test::ExpectTrue(graphics.ApplySettings(ZHLN::COM::InChain(shadowIn, aaIn)).HasValue());

    ZHLN::ShadowSettings shadowOut {};
    ZHLN::AAState        aaOut {};
    ZHLN::Test::ExpectTrue(graphics.GetSettings(ZHLN::COM::OutChain(shadowOut, aaOut)).HasValue());
    ZHLN::Test::ExpectEq(shadowOut.resolution, uint32_t {1024});
    ZHLN::Test::ExpectEq(shadowOut.width, 140.0F);
    ZHLN::Test::ExpectTrue(aaOut.mode == ZHLN::AAMode::FXAA);
    ZHLN::Test::ExpectEq(aaOut.fxaaSubpix, 0.5F);

    ZHLN::GraphicsSettings full = graphics.Settings();
    full.post.exposure          = 1.75F;
    ZHLN::Test::ExpectTrue(graphics.ApplySettings(full).HasValue());
    ZHLN::Test::ExpectEq(graphics.Settings().post.exposure, 1.75F);

    // Buffer creation, update and cleanup.
    const std::array<float, 4> floats {1.0F, 2.0F, 3.0F, 4.0F};
    const auto                 floatBytes = std::as_bytes(std::span<const float> {floats});
    const ZHLN::BufferID       buf        = graphics.CreateBuffer(ZHLN::BufferUsage::Storage, floatBytes, sizeof(float));
    if (buf != ZHLN::BufferID::Invalid) {
        const std::array<float, 4> updated {5.0F, 6.0F, 7.0F, 8.0F};
        ZHLN::Test::ExpectTrue(graphics.UpdateBuffer(buf, std::as_bytes(std::span<const float> {updated})).HasValue());
        graphics.DestroyBuffer(buf);
    }

    // Material creation, registration, lookup and unregistration.
    const auto matRes = graphics.CreateMaterial(ZHLN::COM::PbrMaterialConfig {
        .baseColor = {0.9F, 0.2F, 0.1F, 1.0F},
        .metallic  = 0.1F,
        .roughness = 0.4F,
    });
    if (matRes.HasValue()) {
        const ZHLN::MaterialID matKey = assets.AssetHash("com_boundary_mat");
        ZHLN::Test::ExpectTrue(graphics.RegisterMaterial(matKey, matRes.value).HasValue());
        const ZHLN::COM::Option<ZHLN::Material> foundMat = graphics.FindMaterial(matKey);
        ZHLN::Test::ExpectTrue(foundMat.HasValue());
        graphics.UnregisterMaterial(matKey);
        ZHLN::Test::ExpectTrue(!graphics.FindMaterial(matKey).HasValue());
    }

    // Procedural + raw-stream mesh creation, registration, lookup and destruction.
    const auto boxMeshRes = graphics.CreateBoxMesh({0.5F, 0.5F, 0.5F});
    if (ZHLN::Test::ExpectTrue(boxMeshRes.HasValue())) {
        const ZHLN::AssetID meshKey = assets.AssetHash("com_boundary_box_mesh");
        ZHLN::Test::ExpectTrue(graphics.RegisterMesh(meshKey, boxMeshRes.value).HasValue());
        ZHLN::Test::ExpectTrue(graphics.FindMesh(meshKey).HasValue());
        graphics.UnregisterMesh(meshKey);
        ZHLN::Test::ExpectTrue(!graphics.FindMesh(meshKey).HasValue());
        ZHLN::Test::ExpectTrue(graphics.DestroyMesh(boxMeshRes.value).HasValue());
    }

    const std::array<ZHLN::VertexPosition, 3> triPos {
        ZHLN::VertexPosition {{0.0F, 0.0F, 0.0F}},
        ZHLN::VertexPosition {{1.0F, 0.0F, 0.0F}},
        ZHLN::VertexPosition {{0.0F, 1.0F, 0.0F}},
    };
    const std::array<uint32_t, 3> triIdx {0U, 1U, 2U};
    const auto rawMeshRes = graphics.UploadMesh(ZHLN::COM::MeshStreamsDesc {
        .positions = std::span<const ZHLN::VertexPosition> {triPos},
        .indices32 = std::span<const uint32_t> {triIdx},
    });
    if (ZHLN::Test::ExpectTrue(rawMeshRes.HasValue())) {
        ZHLN::Test::ExpectEq(rawMeshRes.value.vertexCount, uint32_t {3});
        ZHLN::Test::ExpectEq(rawMeshRes.value.indexCount, uint32_t {3});
        ZHLN::Test::ExpectTrue(graphics.DestroyMesh(rawMeshRes.value).HasValue());
    }
}

void CheckSimulation(ZHLN::COM::ISimulation& simulation) {
    ZHLN::Physics::BodyID found[4] {};
    ZHLN::Test::ExpectEq(simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F, found, 4), uint32_t {0});
    ZHLN::Test::ExpectEq(simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F, nullptr, 0), uint32_t {0});

    const ZHLN::COM::Option<ZHLN::DVec3> noPosition = simulation.BodyPosition(ZHLN::Physics::BodyID::Null());
    ZHLN::Test::ExpectTrue(!noPosition.HasValue());
    ZHLN::Test::ExpectEq(noPosition.ValueOr(ZHLN::DVec3 {7.0, 8.0, 9.0}).x, 7.0);
    ZHLN::Test::ExpectTrue(!simulation.BodyState(ZHLN::Physics::BodyID::Null()).HasValue());

    const std::optional<ZHLN::DVec3> asStd = simulation.BodyPosition(ZHLN::Physics::BodyID::Null());
    ZHLN::Test::ExpectTrue(!asStd.has_value());

    const std::vector<ZHLN::Physics::BodyID> nearby = simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F);
    ZHLN::Test::ExpectTrue(nearby.empty());

    const ZHLN::Physics::RaycastResult miss =
        simulation.Raycast(ZHLN::DVec3 {0.0, 100.0, 0.0}, ZHLN::FVec3 {0.0F, -1.0F, 0.0F});
    ZHLN::Test::ExpectTrue(!miss.hasHit);

    // Chained rigid bodies: static floor box, dynamic sphere, and compound shape.
    const auto floorRes = simulation.CreateRigidBody(ZHLN::COM::InChain(
        ZHLN::Physics::BoxShapeDesc {.halfExtents = {10.0F, 0.5F, 10.0F}},
        ZHLN::Physics::RigidBodyPose {
            .position   = {0.0, -0.5, 0.0},
            .motionType = ZHLN::Physics::MotionType::Static,
            .activation = ZHLN::Physics::Activation::DontActivate,
        },
        ZHLN::Physics::RigidBodySurface {
            .materialID = 7, .friction = 0.8F, .restitution = 0.1F, .registerMaterial = true}));
    ZHLN::Test::ExpectTrue(floorRes.HasValue());

    const auto sphereRes = simulation.CreateSphereBody(0.5F, ZHLN::Physics::RigidBodyPose {.position = {0.0, 4.0, 0.0}});
    ZHLN::Test::ExpectTrue(sphereRes.HasValue());

    const std::array<ZHLN::Physics::CompoundChildDesc, 2> compoundParts {
        ZHLN::Physics::CompoundChildDesc::Box({0.25F, 0.25F, 0.25F}, {-0.5F, 0.0F, 0.0F}),
        ZHLN::Physics::CompoundChildDesc::Sphere(0.25F, {0.5F, 0.0F, 0.0F}),
    };
    const auto compoundRes = simulation.CreateRigidBody(ZHLN::COM::InChain(
        ZHLN::Physics::CompoundShapeDesc {
            .children   = compoundParts.data(),
            .childCount = static_cast<uint32_t>(compoundParts.size()),
        },
        ZHLN::Physics::RigidBodyPose {.position = {3.0, 2.0, 0.0}}));
    ZHLN::Test::ExpectTrue(compoundRes.HasValue());

    if (floorRes.HasValue() && sphereRes.HasValue() && compoundRes.HasValue()) {
        simulation.SetBodyLinearVelocity(sphereRes.value, {0.0F, -2.0F, 0.0F});
        simulation.Step(1.0F / 60.0F);
        ZHLN::Test::ExpectTrue(simulation.BodyLinearVelocity(sphereRes.value).HasValue());

        const ZHLN::Physics::RaycastResult hit =
            simulation.Raycast(ZHLN::DVec3 {0.0, 10.0, 0.0}, ZHLN::FVec3 {0.0F, -1.0F, 0.0F});
        ZHLN::Test::ExpectTrue(hit.hasHit);
        ZHLN::Test::ExpectTrue(hit.bodyID == sphereRes.value);

        const auto charRes = simulation.CreateCharacter(ZHLN::COM::InChain(ZHLN::Physics::CharacterCreateDesc {
            .position          = {-3.0, 2.0, 0.0},
            .capsuleHalfHeight = 0.85F,
            .capsuleRadius     = 0.35F,
            .useCustomCapsule  = true,
        }));
        ZHLN::Test::ExpectTrue(charRes.HasValue());

        const std::array<ZHLN::Physics::BodyID, 3> cleanup {floorRes.value, sphereRes.value, compoundRes.value};
        simulation.DestroyBodies(cleanup);
        if (charRes.HasValue()) simulation.DestroyBody(charRes.value);
    }

    const ZHLN::Physics::DebugDrawData debug = simulation.DebugGeometry();
    ZHLN::Test::ExpectTrue(debug.lineCount == 0 || debug.lines != nullptr);
    ZHLN::Test::ExpectTrue(debug.triangleCount == 0 || debug.triangles != nullptr);
    simulation.ClearMeshShapeCache();
}

void CheckAudio(ZHLN::COM::IAudioDevice& audio) {
    ZHLN::Test::ExpectTrue(!audio.IsVoiceValid(ZHLN::VoiceID::Invalid));

    const ZHLN::VoiceID missing = audio.CreateVoice("zahlen://does-not-exist.wav", false);
    ZHLN::Test::ExpectTrue(missing == ZHLN::VoiceID::Invalid || audio.IsVoiceValid(missing));

    audio.SetVoiceVolume(ZHLN::VoiceID::Invalid, 0.5F);
    audio.StopVoice(ZHLN::VoiceID::Invalid, 0.0F);

    const ZHLN::AudioEvent event {.type = ZHLN::AudioEventType::OneShot2D, .volume = 0.25F};
    ZHLN::Test::ExpectTrue(audio.PostEvent(event).HasValue());

    ZHLN::Test::ExpectTrue(audio.Initialized() || !audio.Initialized());
}

void CheckAssets(ZHLN::COM::IAssetLibrary& assets, ZHLN::COM::IEntities& entities) {
    const uint64_t hash = assets.PrefabHash("zahlen://models/not-here.glb");
    ZHLN::Test::ExpectTrue(hash != 0);
    ZHLN::Test::ExpectEq(assets.PrefabHash("zahlen://models/not-here.glb"), hash);
    ZHLN::Test::ExpectTrue(!assets.HasPrefab(hash));
    ZHLN::Test::ExpectTrue(assets.AssetHash("materials/Brick") != ZHLN::InvalidAssetID);

    const auto missing = assets.FindPrefab("zahlen://models/not-here.glb");
    ZHLN::Test::ExpectTrue(!missing.HasValue());
    ZHLN::Test::ExpectTrue(missing.error == ZHLN::ErrorCode(ZHLN::COM::ComError::AssetNotCached));

    const ZHLN::COM::Option<ZHLN::COM::PrefabInfo> info = assets.DescribePrefab(hash);
    ZHLN::Test::ExpectTrue(!info.HasValue());

    char path[64] {};
    ZHLN::Test::ExpectEq(assets.PrefabVirtualPath(hash, path, sizeof(path)), uint32_t {0});
    ZHLN::Test::ExpectTrue(!assets.FindEnvironmentImage("no-such-sky").HasValue());

    const std::optional<ZHLN::COM::EnvironmentImageInfo> asStd = assets.FindEnvironmentImage("no-such-sky");
    ZHLN::Test::ExpectTrue(!asStd.has_value());

    ZHLN::Test::ExpectTrue(assets.PrefabVirtualPath(hash).empty());
    ZHLN::Test::ExpectTrue(assets.Instantiate(hash).empty());
    ZHLN::Test::ExpectEq(assets.EnvironmentPixels("no-such-sky").size, size_t {0});

    // Spawn a procedural box primitive with physics over COM and verify its
    // published components are attached and queryable.
    ZHLN::PrefabFactory::SpawnParams spawnParams {};
    spawnParams.position      = {1.0, 3.0, -2.0};
    spawnParams.color         = {0.2F, 0.7F, 0.9F, 1.0F};
    spawnParams.createPhysics = true;
    const auto boxEnt         = assets.SpawnBox({0.5F, 0.5F, 0.5F}, spawnParams);
    if (ZHLN::Test::ExpectTrue(boxEnt.HasValue())) {
        ZHLN::Test::ExpectTrue(entities.IsAlive(boxEnt.value));
        ZHLN::Test::ExpectTrue(entities.GetEngineRaw(boxEnt.value, "ZHLN::Components::MeshComponent") != nullptr);
        ZHLN::Test::ExpectTrue(entities.GetEngineRaw(boxEnt.value, "ZHLN::Components::OwnedMeshComponent") != nullptr);
        ZHLN::Test::ExpectTrue(entities.GetEngineRaw(boxEnt.value, "ZHLN::Components::PhysicsComponent") != nullptr);
        entities.Despawn(boxEnt.value);
    }
}

void CheckActions(ZHLN::COM::IActionContext& actions) {
    actions.BeginFrame();

    actions.InjectKey(ZHLN::KeyCode::W, true, ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectTrue(actions.KeyDown(ZHLN::KeyCode::W, ZHLN::kPrimarySeat));
    ZHLN::Test::ExpectTrue(actions.KeyPressed(ZHLN::KeyCode::W, ZHLN::kPrimarySeat));
    ZHLN::Test::ExpectTrue(!actions.KeyDown(ZHLN::KeyCode::S, ZHLN::kPrimarySeat));

    actions.InjectKey(ZHLN::KeyCode::W, false, ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectTrue(!actions.KeyDown(ZHLN::KeyCode::W, ZHLN::kPrimarySeat));
    ZHLN::Test::ExpectTrue(actions.KeyReleased(ZHLN::KeyCode::W, ZHLN::kPrimarySeat));

    actions.InjectMouseMove(40.0F, 25.0F, ZHLN::kPrimarySeat);
    const ZHLN::Axis2DValue mouse = actions.MousePosition(ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectEq(mouse.x, 40.0F);
    ZHLN::Test::ExpectEq(mouse.y, 25.0F);

    actions.InjectMouseScroll(2.0F, ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectEq(actions.MouseWheel(ZHLN::kPrimarySeat), 2.0F);

    ZHLN::Test::ExpectEq(actions.ConnectedGamepadCount(), uint32_t {0});
    actions.InjectGamepadConnection(0, true, "Boundary Test Pad");
    ZHLN::Test::ExpectTrue(actions.GamepadConnected(0));
    ZHLN::Test::ExpectEq(actions.ConnectedGamepadCount(), uint32_t {1});

    actions.InjectGamepadButton(0, ZHLN::GamepadButton::South, true);
    ZHLN::Test::ExpectTrue(actions.GamepadButtonDown(0, ZHLN::GamepadButton::South));
    ZHLN::Test::ExpectTrue(!actions.GamepadButtonDown(0, ZHLN::GamepadButton::North));

    actions.InjectGamepadAxis(0, ZHLN::GamepadAxis::LeftTrigger, 1.0F);
    ZHLN::Test::ExpectTrue(actions.GamepadAxisValue(0, ZHLN::GamepadAxis::LeftTrigger) > 0.5F);

    ZHLN::Test::ExpectTrue(!actions.GamepadConnected(3));
    ZHLN::Test::ExpectEq(actions.GamepadAxisValue(3, ZHLN::GamepadAxis::LeftTrigger), 0.0F);

    actions.RefreshActions();
    const ZHLN::ActionState unbound = actions.Action(ZHLN::ActionID {"NothingBindsThis"}, ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectTrue(!unbound.down);
    ZHLN::Test::ExpectEq(unbound.x, 0.0F);
    ZHLN::Test::ExpectTrue(!actions.ActionDown(ZHLN::ActionID {"NothingBindsThis"}));
    ZHLN::Test::ExpectEq(actions.ActionAxis2D(ZHLN::ActionID {"NothingBindsThis"}).x, 0.0F);

    // Chained action-map creation, binding, rebinding, defaults reset and routing.
    ZHLN::Test::ExpectTrue(actions.CreateActionMap("Gameplay", 10, true).HasValue());
    ZHLN::Test::ExpectTrue(actions.HasActionMap("Gameplay"));
    ZHLN::Test::ExpectTrue(actions.IsActionMapEnabled("Gameplay"));

    const std::array<ZHLN::InputBinding, 1> jumpBindings {ZHLN::InputBinding::Key(ZHLN::KeyCode::Space)};
    const auto jumpRes = actions.AddAction("Gameplay", "Jump", ZHLN::ActionKind::Digital, jumpBindings);
    if (ZHLN::Test::ExpectTrue(jumpRes.HasValue())) {
        const ZHLN::ActionID jumpId = jumpRes.value;
        ZHLN::Test::ExpectTrue(
            actions.AddBinding("Gameplay", jumpId, ZHLN::InputBinding::GamepadBtn(ZHLN::GamepadButton::North))
                .HasValue());
        actions.SnapshotDefaults();

        actions.BeginFrame();
        actions.InjectKey(ZHLN::KeyCode::Space, true, ZHLN::kPrimarySeat);
        actions.RefreshActions();
        ZHLN::Test::ExpectTrue(actions.ActionDown(jumpId, ZHLN::kPrimarySeat));
        ZHLN::Test::ExpectTrue(actions.ActionPressed(jumpId, ZHLN::kPrimarySeat));

        actions.BeginFrame();
        actions.InjectKey(ZHLN::KeyCode::Space, false, ZHLN::kPrimarySeat);
        actions.RefreshActions();
        ZHLN::Test::ExpectTrue(actions.ActionReleased(jumpId, ZHLN::kPrimarySeat));

        ZHLN::Test::ExpectTrue(actions.Rebind("Gameplay", jumpId, 0U, ZHLN::InputBinding::Key(ZHLN::KeyCode::E)).HasValue());
        ZHLN::Test::ExpectTrue(actions.ResetActionMapToDefaults("Gameplay"));
        ZHLN::Test::ExpectTrue(actions.ClearBindings("Gameplay", jumpId));
    }

    ZHLN::Test::ExpectTrue(actions.SetRoutingPolicy(ZHLN::SeatRoutingPolicy::DedicatedGamepadSeats).HasValue());
    ZHLN::Test::ExpectTrue(actions.SetKeyboardMouseSeat(1, true).HasValue());
    ZHLN::Test::ExpectTrue(actions.RoutingPolicy() == ZHLN::SeatRoutingPolicy::DedicatedGamepadSeats);

    ZHLN::InputRoutingConfig routingOut {};
    ZHLN::Test::ExpectTrue(actions.GetRouting(ZHLN::COM::OutChain(routingOut)).HasValue());
    ZHLN::Test::ExpectTrue((routingOut.keyboardMouseSeatMask & (1U << 1)) != 0U);

    ZHLN::Test::ExpectTrue(actions.RemoveActionMap("Gameplay"));
    ZHLN::Test::ExpectTrue(!actions.HasActionMap("Gameplay"));
}

void CheckEngineComponents(ZHLN::COM::IEntities& entities, ZHLN::COM::ISimulation& simulation) {
    const ZHLN::Entity entity = entities.CreateEntity();

    // All 34 fixed-layout engine components are published; CSGComponent (with std::vector) is not.
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::TransformComponent"));
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::WorldTransformComponent"));
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::PBRComponent"));
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::LightComponent"));
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::PhysicsComponent"));
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::AudioSourceComponent"));
    ZHLN::Test::ExpectTrue(!entities.IsEngineComponent("ZHLN::Components::CSGComponent"));
    ZHLN::Test::ExpectTrue(!entities.IsEngineComponent("ZHLN::Components::NotAComponentAnywhere"));

    ZHLN::Test::ExpectTrue(entities.GetEngineRaw(entity, "ZHLN::Components::WorldTransformComponent") == nullptr);

    ConsumerSide::WorldTransformComponent transform {};
    transform.world.m[0]    = 2.0F;
    transform.previous.m[5] = 3.0F;

    void* attached = entities.AttachEngineRaw(entity, "ZHLN::Components::WorldTransformComponent",
                                              sizeof(ConsumerSide::WorldTransformComponent),
                                              alignof(ConsumerSide::WorldTransformComponent),
                                              ZHLN::Reflect::LayoutHash<ConsumerSide::WorldTransformComponent>());
    if (ZHLN::Test::ExpectTrue(attached != nullptr)) {
        *static_cast<ConsumerSide::WorldTransformComponent*>(attached) = transform;

        auto* fetched = static_cast<ConsumerSide::WorldTransformComponent*>(
            entities.GetEngineRaw(entity, "ZHLN::Components::WorldTransformComponent"));
        if (ZHLN::Test::ExpectTrue(fetched != nullptr)) {
            ZHLN::Test::ExpectEq(fetched->world.m[0], 2.0F);
            ZHLN::Test::ExpectEq(fetched->previous.m[5], 3.0F);
        }
        ZHLN::Test::ExpectTrue(!entities.EntitiesWith("ZHLN::Components::WorldTransformComponent").empty());
    }

    // SceneResources ownership safety: detaching PhysicsComponent over COM must
    // destroy the underlying Jolt body rather than leaking it.
    const auto sphereBody = simulation.CreateSphereBody(0.5F, ZHLN::Physics::RigidBodyPose {.position = {0.0, 8.0, 0.0}});
    if (ZHLN::Test::ExpectTrue(sphereBody.HasValue())) {
        void* physSlot = entities.AttachEngineRaw(entity, "ZHLN::Components::PhysicsComponent",
                                                  sizeof(ConsumerSide::PhysicsComponent),
                                                  alignof(ConsumerSide::PhysicsComponent),
                                                  ZHLN::Reflect::LayoutHash<ConsumerSide::PhysicsComponent>());
        if (ZHLN::Test::ExpectTrue(physSlot != nullptr)) {
            *static_cast<ConsumerSide::PhysicsComponent*>(physSlot) =
                ConsumerSide::PhysicsComponent {.physicsID = sphereBody.value, .isStatic = false};
            ZHLN::Test::ExpectTrue(entities.DetachEngineRaw(entity, "ZHLN::Components::PhysicsComponent"));
            ZHLN::Test::ExpectTrue(!simulation.BodyPosition(sphereBody.value).HasValue());
        }
    }

    // Mismatched size or hash is refused closed.
    void* mismatched = entities.AttachEngineRaw(entity, "ZHLN::Components::MeshComponent",
                                                sizeof(ConsumerSide::MeshComponent),
                                                alignof(ConsumerSide::MeshComponent),
                                                ZHLN::Reflect::LayoutHash<ConsumerSide::MeshComponent>());
    ZHLN::Test::ExpectTrue(mismatched == nullptr);

    void* sameSizeLie = entities.AttachEngineRaw(entity, "ZHLN::Components::WorldTransformComponent",
                                                 sizeof(ConsumerSide::WorldTransformComponent),
                                                 alignof(ConsumerSide::WorldTransformComponent),
                                                 ZHLN::Reflect::LayoutHash<ConsumerSide::WorldTransformComponent>() ^ 1ULL);
    ZHLN::Test::ExpectTrue(sameSizeLie == nullptr);

    ZHLN::Test::ExpectTrue(entities.DetachEngineRaw(entity, "ZHLN::Components::WorldTransformComponent"));
    ZHLN::Test::ExpectTrue(entities.GetEngineRaw(entity, "ZHLN::Components::WorldTransformComponent") == nullptr);
    ZHLN::Test::ExpectTrue(!entities.DetachEngineRaw(entity, "ZHLN::Components::MeshComponent"));

    entities.Despawn(entity);
}

struct COMBoundarySuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> create_engine_validates_config_chains_before_device_init() {
            const auto unknownRes =
                ZHLN::COM::CreateEngine(ZHLN::COM::InChain(COMIdentityTest::UnknownEngineSlice {}));
            ZHLN::Test::ExpectTrue(!unknownRes.HasValue());
            ZHLN::Test::ExpectTrue(unknownRes.error.Is(ZHLN::COM::ComError::UnsupportedExtension));

            ZHLN::COM::EngineWindowConfig winPayload {.headless = true};
            ZHLN::COM::ChainInNode        drifted {
                .sType      = ZHLN::COM::ChainTypeID<ZHLN::COM::EngineWindowConfig>(),
                .layoutHash = ZHLN::Reflect::LayoutHash<ZHLN::COM::EngineWindowConfig>() ^ 0x1ULL,
                .size       = sizeof(ZHLN::COM::EngineWindowConfig),
                .reserved   = 0,
                .data       = &winPayload,
                .pNext      = nullptr,
            };
            const auto driftRes = ZHLN::COM::CreateEngine(&drifted);
            ZHLN::Test::ExpectTrue(!driftRes.HasValue());
            ZHLN::Test::ExpectTrue(driftRes.error.Is(ZHLN::COM::ComError::LayoutMismatch));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> the_com_boundary_works_end_to_end() {
            auto created = ZHLN::COM::CreateEngineRef(ZHLN::COM::InChain(
                ZHLN::COM::EngineAppConfig {
                    .appName  = ZHLN::String64("COM boundary test"),
                    .logLevel = ZHLN::LogLevel::Quiet,
                },
                ZHLN::COM::EngineWindowConfig {
                    .width    = 320,
                    .height   = 240,
                    .vsync    = false,
                    .headless = true,
                },
                ZHLN::COM::EngineRenderConfig {
                    .validationMode = ZHLN::ValidationMode::Off,
                }));
            if (!created.has_value()) {
                ZHLN::Test::ExpectTrue(created.error() != ZHLN::ErrorCode {});
                return {};
            }

            ZHLN::COM::Ref<ZHLN::COM::IEngine> engine = std::move(*created);
            ZHLN::Test::ExpectTrue(static_cast<bool>(engine));

            ZHLN::COM::Ref<ZHLN::COM::IGraphicsDevice> graphics   = engine.As<ZHLN::COM::IGraphicsDevice>();
            ZHLN::COM::Ref<ZHLN::COM::IEntities>       entities   = engine.As<ZHLN::COM::IEntities>();
            ZHLN::COM::Ref<ZHLN::COM::ISimulation>     simulation = engine.As<ZHLN::COM::ISimulation>();
            ZHLN::COM::Ref<ZHLN::COM::IAudioDevice>    audio      = engine.As<ZHLN::COM::IAudioDevice>();
            ZHLN::COM::Ref<ZHLN::COM::IAssetLibrary>   assets     = engine.As<ZHLN::COM::IAssetLibrary>();
            ZHLN::COM::Ref<ZHLN::COM::IActionContext>  actions    = engine.As<ZHLN::COM::IActionContext>();
            if (!ZHLN::Test::ExpectTrue(static_cast<bool>(graphics)) || !ZHLN::Test::ExpectTrue(static_cast<bool>(entities)) ||
                !ZHLN::Test::ExpectTrue(static_cast<bool>(simulation)) || !ZHLN::Test::ExpectTrue(static_cast<bool>(audio)) ||
                !ZHLN::Test::ExpectTrue(static_cast<bool>(assets)) || !ZHLN::Test::ExpectTrue(static_cast<bool>(actions))) {
                return std::unexpected(BoundaryTestError::QueryFailed);
            }
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Graphics()), static_cast<void*>(graphics.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Entities()), static_cast<void*>(entities.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Simulation()), static_cast<void*>(simulation.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Audio()), static_cast<void*>(audio.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Assets()), static_cast<void*>(assets.Get()));
            ZHLN::Test::ExpectEq(static_cast<void*>(engine->Actions()), static_cast<void*>(actions.Get()));

            ZHLN::COM::IObject* root = engine.Get();
            ZHLN::Test::ExpectEq(root->AddRef(), uint32_t {8});
            ZHLN::Test::ExpectEq(root->Release(), uint32_t {7});

            const ZHLN::ViewportRect viewport = graphics->Viewport();
            ZHLN::Test::ExpectTrue(viewport.width >= 0.0f);
            ZHLN::Test::ExpectTrue(engine->IsRunning() || !engine->IsRunning());

            const ZHLN::Entity entity = entities->CreateEntity();
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));

            ClientTransform* attached = entities->Attach(entity, ClientTransform {.position = {1.0F, 2.0F, 3.0F}, .scale = 4.0F});
            if (ZHLN::Test::ExpectTrue(attached != nullptr)) {
                ZHLN::Test::ExpectEq(entities->EntitiesWith<ClientTransform>().size(), size_t {1});
                ZHLN::Test::ExpectTrue(entities->SingletonEntity<ClientTransform>().HasValue());
                ClientTransform* fetched = entities->Get<ClientTransform>(entity);
                if (ZHLN::Test::ExpectTrue(fetched != nullptr)) {
                    ZHLN::Test::ExpectEq(fetched->position[1], 2.0F);
                    ZHLN::Test::ExpectEq(fetched->scale, 4.0F);
                }
                ZHLN::Test::ExpectTrue(entities->Detach<ClientTransform>(entity));
                ZHLN::Test::ExpectTrue(entities->Get<ClientTransform>(entity) == nullptr);
            }

            using TemplateComponent      = COMNameTest::Component<int, 2>;
            using OtherTemplateComponent = COMNameTest::Component<int, 3>;
            auto* templateAttached       = entities->Attach(entity, TemplateComponent {{11, 22}});
            auto* otherAttached          = entities->Attach(entity, OtherTemplateComponent {{33, 44, 55}});
            if (ZHLN::Test::ExpectTrue(templateAttached != nullptr && otherAttached != nullptr)) {
                auto* first  = entities->Get<TemplateComponent>(entity);
                auto* second = entities->Get<OtherTemplateComponent>(entity);
                if (ZHLN::Test::ExpectTrue(first != nullptr && second != nullptr)) {
                    ZHLN::Test::ExpectEq(first->value[1], 22);
                    ZHLN::Test::ExpectEq(second->value[2], 55);
                }
                ZHLN::Test::ExpectTrue(entities->Detach<TemplateComponent>(entity));
                ZHLN::Test::ExpectTrue(entities->Get<OtherTemplateComponent>(entity) != nullptr);
            }

            CheckEngineComponents(*entities.Get(), *simulation.Get());

            entities->Despawn(entity);
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));

            CheckGraphics(*graphics.Get(), *assets.Get());
            CheckSimulation(*simulation.Get());
            CheckAudio(*audio.Get());
            CheckAssets(*assets.Get(), *entities.Get());
            CheckActions(*actions.Get());

            auto state = engine.As<ZHLN::COM::IState>();
            if (ZHLN::Test::ExpectTrue(static_cast<bool>(state) && state.Get() == engine->State())) {
                ZHLN::Test::ExpectTrue(state->CullingStats().EnableCulling);
                ZHLN::Test::ExpectFalse(state->CullingStats().FreezeFrustum);
                state->SetCullingEnabled(false);
                state->SetFreezeFrustum(true);
                ZHLN::Test::ExpectFalse(state->CullingStats().EnableCulling);
                ZHLN::Test::ExpectTrue(state->CullingStats().FreezeFrustum);
                state->ResetCullingStats();
                ZHLN::Test::ExpectTrue(state->CullingStats().EnableCulling);
                ZHLN::Test::ExpectFalse(state->CullingStats().FreezeFrustum);
            }

            ZHLN::Test::ExpectTrue(entities->SingletonEntity("ZHLN::Components::MainCameraTagComponent").HasValue());
            engine->ClearScene();
            ZHLN::Test::ExpectTrue(entities->SingletonEntity("ZHLN::Components::MainCameraTagComponent").HasValue());
            return {};
        }
    };
};

} // namespace
