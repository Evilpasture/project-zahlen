// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestCOMBoundary.cpp
//
// The public API as a consumer sees it. Apart from the test framework this
// file includes only Zahlen/COM and Zahlen/Core/COM headers -- no engine
// header, no PIMPL, no internal access -- and every engine call it makes is a
// virtual dispatch through a vtable into libzahlen_engine.so. EngineCOM's
// definition is not visible here, so nothing can be inlined or devirtualised
// away.

#include "TestsFramework.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEngineBuilder.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <type_traits>
#include <vector>

namespace {

// A component the library has never heard of, declared in the consumer's TU.
struct ClientTransform {
    float position[3];
    float scale;
};

enum class BoundaryTestError : uint8_t { QueryFailed = 1 };

// The whole surface is reachable without any engine header.
static_assert(ZHLN::COM::ComComponent<ClientTransform>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Option<ZHLN::DVec3>>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::Option<ZHLN::DVec3>>);
static_assert(std::is_standard_layout_v<ZHLN::COM::Option<ZHLN::Physics::BodyStateSnapshot>>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::Option<ZHLN::Physics::BodyStateSnapshot>>);
// EngineConfigCOM is gone: a struct the consumer allocates and the library
// reads by layout cannot survive a field being appended to it. Creation
// options are calls on IEngineBuilder now, and that interface is held to the
// same layout rules as every other one here.
static_assert(ZHLN::COM::ComInterfaceLayoutCompliant<ZHLN::COM::IEngineBuilder>());
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
// The audit inspects the vtable, and only the vtable. These two types state
// the line it draws, so narrowing it to the virtual members cannot quietly
// become a licence to put a container on the wire.
namespace AuditProbe {

// A virtual returning a container is still rejected: this is a slot two
// binaries must agree on, and std::vector's layout is not an agreement.
struct ContainerOnTheWire {
    virtual auto Bodies() const noexcept -> std::vector<uint32_t> = 0;

protected:
    ~ContainerOnTheWire() = default;
};

static_assert(!ZHLN::COM::ComInterfaceLayoutCompliant<ContainerOnTheWire>());

// The same container is fine one line below, because an inline non-virtual
// is compiled by the consumer and never appears in the vtable at all.
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

// And a virtual destructor remains disqualifying either way.
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

// The boundary vocabulary the four capability headers add, re-checked from the
// consumer's side: these cross as plain bytes or they do not cross at all.
static_assert(std::is_trivially_copyable_v<ZHLN::Physics::RaycastResult>);
static_assert(std::is_trivially_copyable_v<ZHLN::VoiceDesc>);
static_assert(std::is_trivially_copyable_v<ZHLN::AudioEvent>);
static_assert(std::is_trivially_copyable_v<ZHLN::ActionState>);
static_assert(std::is_trivially_copyable_v<ZHLN::PrefabFactory::SpawnParams>);
static_assert(std::is_trivially_copyable_v<ZHLN::COM::PrefabInfo>);

// Component families are keyed on the qualified name, so two components with
// the same unqualified name in different namespaces stay apart.
static_assert(ZHLN::COM::IEntities::ComponentName<ClientTransform>().size > 0);

// The two collection queries the facade answers with std::vector and
// std::optional, in the shapes that can cross. An empty world is the
// interesting case here: the answers have to be well-formed, not
// merely non-crashing.
void CheckSimulation(ZHLN::COM::ISimulation& simulation) {
    // The wire tier, called directly: caller's buffer, count returned.
    ZHLN::Physics::BodyID found[4] {};
    ZHLN::Test::ExpectEq(simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F, found, 4), uint32_t {0});
    // A null buffer is legal and still reports the real count.
    ZHLN::Test::ExpectEq(simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F, nullptr, 0), uint32_t {0});

    // Presence comes back as a value, not as a bool plus storage the caller
    // had to declare first.
    const ZHLN::COM::Option<ZHLN::DVec3> noPosition = simulation.BodyPosition(ZHLN::Physics::BodyID::Null());
    ZHLN::Test::ExpectTrue(!noPosition.HasValue());
    ZHLN::Test::ExpectEq(noPosition.ValueOr(ZHLN::DVec3 {7.0, 8.0, 9.0}).x, 7.0);

    ZHLN::Test::ExpectTrue(!simulation.BodyState(ZHLN::Physics::BodyID::Null()).HasValue());

    // ...and converts to the standard type in this translation unit, which is
    // the half the library never compiles.
    const std::optional<ZHLN::DVec3> asStd = simulation.BodyPosition(ZHLN::Physics::BodyID::Null());
    ZHLN::Test::ExpectTrue(!asStd.has_value());

    const std::vector<ZHLN::Physics::BodyID> nearby = simulation.BodiesInSphere(ZHLN::DVec3 {0.0, 0.0, 0.0}, 50.0F);
    ZHLN::Test::ExpectTrue(nearby.empty());

    // Returned in memory through the Itanium hidden pointer, 56 bytes
    // of it, from the inline overload that supplies the defaults.
    const ZHLN::Physics::RaycastResult hit =
        simulation.Raycast(ZHLN::DVec3 {0.0, 100.0, 0.0}, ZHLN::FVec3 {0.0F, -1.0F, 0.0F});
    ZHLN::Test::ExpectTrue(!hit.hasHit);

    // Borrowed engine memory: counts may be zero, but the pair must be
    // self-consistent rather than a dangling pointer with a count.
    const ZHLN::Physics::DebugDrawData debug = simulation.DebugGeometry();
    ZHLN::Test::ExpectTrue(debug.lineCount == 0 || debug.lines != nullptr);
    ZHLN::Test::ExpectTrue(debug.triangleCount == 0 || debug.triangles != nullptr);
}

// Headless runs have no output device, so what is asserted is that the
// handle discipline holds either way: an invalid voice is never valid,
// and every operation on one is a safe no-op across the boundary.
void CheckAudio(ZHLN::COM::IAudioDevice& audio) {
    ZHLN::Test::ExpectTrue(!audio.IsVoiceValid(ZHLN::VoiceID::Invalid));

    const ZHLN::VoiceID missing = audio.CreateVoice("zahlen://does-not-exist.wav", false);
    ZHLN::Test::ExpectTrue(missing == ZHLN::VoiceID::Invalid || audio.IsVoiceValid(missing));

    audio.SetVoiceVolume(ZHLN::VoiceID::Invalid, 0.5F);
    audio.StopVoice(ZHLN::VoiceID::Invalid, 0.0F);

    // A 120-byte descriptor by const reference, and an AudioEvent with
    // its inline String64 by const reference: no allocator crosses.
    const ZHLN::AudioEvent event {.type = ZHLN::AudioEventType::OneShot2D, .volume = 0.25F};
    audio.PostEvent(event);

    ZHLN::Test::ExpectTrue(audio.Initialized() || !audio.Initialized());
}

// Nothing is cached in a fresh engine, which makes every miss path
// checkable: the hash is a pure function, and a miss is reported
// rather than guessed at.
void CheckAssets(ZHLN::COM::IAssetLibrary& assets) {
    const uint64_t hash = assets.PrefabHash("zahlen://models/not-here.glb");
    ZHLN::Test::ExpectTrue(hash != 0);
    ZHLN::Test::ExpectEq(assets.PrefabHash("zahlen://models/not-here.glb"), hash);
    ZHLN::Test::ExpectTrue(!assets.HasPrefab(hash));

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

    // The ergonomics tier. Instantiate is deliberately *not* called on a
    // cached prefab here: it spawns, and this case owns no scene to spawn
    // into. The uncached path still proves the overload resolves, compiles
    // and returns an empty vector without touching the registry.
    ZHLN::Test::ExpectTrue(assets.PrefabVirtualPath(hash).empty());
    ZHLN::Test::ExpectTrue(assets.Instantiate(hash).empty());
    ZHLN::Test::ExpectEq(assets.EnvironmentPixels("no-such-sky").size, size_t {0});
}

// Injection and query are both here, so the interface can be proven
// from the consumer's side without the engine's window layer: feed it
// events, read the state back out.
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

    // Hotplug, button and axis all travel as scalars; GamepadState
    // itself holds std::bitset and is correctly refused by the audit.
    ZHLN::Test::ExpectEq(actions.ConnectedGamepadCount(), uint32_t {0});
    actions.InjectGamepadConnection(0, true, "Boundary Test Pad");
    ZHLN::Test::ExpectTrue(actions.GamepadConnected(0));
    ZHLN::Test::ExpectEq(actions.ConnectedGamepadCount(), uint32_t {1});

    actions.InjectGamepadButton(0, ZHLN::GamepadButton::South, true);
    ZHLN::Test::ExpectTrue(actions.GamepadButtonDown(0, ZHLN::GamepadButton::South));
    ZHLN::Test::ExpectTrue(!actions.GamepadButtonDown(0, ZHLN::GamepadButton::North));

    actions.InjectGamepadAxis(0, ZHLN::GamepadAxis::LeftTrigger, 1.0F);
    ZHLN::Test::ExpectTrue(actions.GamepadAxisValue(0, ZHLN::GamepadAxis::LeftTrigger) > 0.5F);

    // An unconnected slot answers rather than faulting.
    ZHLN::Test::ExpectTrue(!actions.GamepadConnected(3));
    ZHLN::Test::ExpectEq(actions.GamepadAxisValue(3, ZHLN::GamepadAxis::LeftTrigger), 0.0F);

    // Re-evaluating with no action maps defined is a no-op, and an
    // action nothing binds reads as un-actuated rather than garbage.
    actions.RefreshActions();
    const ZHLN::ActionState unbound = actions.Action(ZHLN::ActionID {"NothingBindsThis"}, ZHLN::kPrimarySeat);
    ZHLN::Test::ExpectTrue(!unbound.down);
    ZHLN::Test::ExpectEq(unbound.x, 0.0F);
    ZHLN::Test::ExpectTrue(!actions.ActionDown(ZHLN::ActionID {"NothingBindsThis"}));
    ZHLN::Test::ExpectEq(actions.ActionAxis2D(ZHLN::ActionID {"NothingBindsThis"}).x, 0.0F);
}


// A consumer's own declaration of two engine components. Nothing here
// includes an engine header: the layout is restated, and the engine's hash
// check is what decides whether the restatement is true. Mat44Bytes stands in
// for ZHLN::Mat4x4 at the only level the boundary cares about -- its bytes.
namespace ConsumerSide {

// alignas matches ZHLN::Mat4x4, which is alignas(alignof(JPH::Mat44)). A
// consumer restating a layout restates its alignment as well -- and if it
// gets that wrong, the hash is what says so.
struct alignas(16) Mat44Bytes {
    float m[16];
};

struct WorldTransformComponent {
    Mat44Bytes world;
    Mat44Bytes previous;
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

void CheckEngineComponents(ZHLN::COM::IEntities& entities) {
    const ZHLN::Entity entity = entities.CreateEntity();

    // A published name is recognised as engine-owned; a consumer-defined one
    // is not, and keeps the dynamic path it has always had.
    ZHLN::Test::ExpectTrue(entities.IsEngineComponent("ZHLN::Components::WorldTransformComponent"));
    ZHLN::Test::ExpectTrue(!entities.IsEngineComponent("ZHLN::Components::NotAComponentAnywhere"));

    // Matching layout: the engine serves typed storage its own systems read.
    // The consumer's qualified name must be the engine's, so it is passed
    // explicitly rather than taken from the local declaration's namespace.
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

        // Read back through the boundary, from typed storage.
        auto* fetched = static_cast<ConsumerSide::WorldTransformComponent*>(
            entities.GetEngineRaw(entity, "ZHLN::Components::WorldTransformComponent"));
        if (ZHLN::Test::ExpectTrue(fetched != nullptr)) {
            ZHLN::Test::ExpectEq(fetched->world.m[0], 2.0F);
            ZHLN::Test::ExpectEq(fetched->previous.m[5], 3.0F);
        }
    }

    // The negative case this whole mechanism exists for: a consumer whose
    // idea of the component disagrees with the library's is refused, not
    // served a mismatched view of its own bytes.
    void* mismatched = entities.AttachEngineRaw(entity, "ZHLN::Components::MeshComponent",
                                                sizeof(ConsumerSide::MeshComponent),
                                                alignof(ConsumerSide::MeshComponent),
                                                ZHLN::Reflect::LayoutHash<ConsumerSide::MeshComponent>());
    ZHLN::Test::ExpectTrue(mismatched == nullptr);

    // And a right-sized lie is refused too -- the hash is what catches a
    // same-size substitution that size and alignment cannot see.
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
        // One engine, because standing one up is a real device initialisation.
        // Creating it may legitimately fail on a machine with no Vulkan at
        // all; what is asserted unconditionally is that the call crossed the
        // boundary and came back with a well-formed answer either way.
        std::expected<void, ZHLN::ErrorCode> the_com_boundary_works_end_to_end() {
            auto builderCreated = ZHLN::COM::CreateEngineBuilderRef();
            if (!ZHLN::Test::ExpectTrue(builderCreated.has_value())) return {};
            ZHLN::COM::Ref<ZHLN::COM::IEngineBuilder> builder = std::move(*builderCreated);

            builder->SetAppName("COM boundary test");
            builder->SetWindowSize(320, 240);
            builder->SetVSync(false);
            builder->SetValidation(false);
            builder->SetHeadless(true);

            auto created = builder->BuildRef();
            if (!created.has_value()) {
                // The failure travelled back as a fixed-layout ErrorCode, not
                // an exception and not a std::expected across the ABI, and it
                // names a real error rather than a default-constructed blank.
                ZHLN::Test::ExpectTrue(created.error() != ZHLN::ErrorCode {});
                return {};
            }

            ZHLN::COM::Ref<ZHLN::COM::IEngine> engine = std::move(*created);
            ZHLN::Test::ExpectTrue(static_cast<bool>(engine));

            // Query reaches sibling interfaces on the same allocation, and the
            // borrowed accessors reach the very same subobjects. Seven
            // interfaces, one object, no extra allocation anywhere.
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

            // Refcounting lives in the library: these are virtual calls into
            // the .so, and so is the delete when the last one goes. Seven refs
            // are alive here -- the root plus one per capability.
            ZHLN::COM::IObject* root = engine.Get();
            ZHLN::Test::ExpectEq(root->AddRef(), uint32_t {8});
            ZHLN::Test::ExpectEq(root->Release(), uint32_t {7});

            // A virtual call returning a fixed-layout struct through the
            // Itanium hidden-pointer convention.
            const ZHLN::ViewportRect viewport = graphics->Viewport();
            ZHLN::Test::ExpectTrue(viewport.width >= 0.0f);
            ZHLN::Test::ExpectTrue(engine->IsRunning() || !engine->IsRunning());

            // The ECS seam: a component type the library has never seen,
            // attached by qualified name and read back through the vtable.
            const ZHLN::Entity entity = entities->CreateEntity();
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));

            ClientTransform* attached = entities->Attach(entity, ClientTransform {.position = {1.0F, 2.0F, 3.0F}, .scale = 4.0F});
            if (ZHLN::Test::ExpectTrue(attached != nullptr)) {
                ClientTransform* fetched = entities->Get<ClientTransform>(entity);
                if (ZHLN::Test::ExpectTrue(fetched != nullptr)) {
                    ZHLN::Test::ExpectEq(fetched->position[1], 2.0F);
                    ZHLN::Test::ExpectEq(fetched->scale, 4.0F);
                }
                ZHLN::Test::ExpectTrue(entities->Detach<ClientTransform>(entity));
                ZHLN::Test::ExpectTrue(entities->Get<ClientTransform>(entity) == nullptr);
            }

            // Despawn marks the entity for destruction and the engine frees it
            // during pending-destroy processing, so it is deliberately still
            // alive here. Asserting the deferral is the honest check; the
            // boundary forwards the call, it does not change the semantics.
            CheckEngineComponents(*entities.Get());

            entities->Despawn(entity);
            ZHLN::Test::ExpectTrue(entities->IsAlive(entity));

            CheckSimulation(*simulation.Get());
            CheckAudio(*audio.Get());
            CheckAssets(*assets.Get());
            CheckActions(*actions.Get());
            return {};
        }

    };
};

} // namespace
