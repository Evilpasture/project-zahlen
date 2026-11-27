// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include <engine/Hierarchy.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/ECS/Components.hpp>
#include "LogBridge.hpp"
#include <ecs/ECS.hpp>
#include <engine/World.hpp>
#include <physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <string_view>
#include <unordered_set>
#include <utility>
namespace ZHLN::COM {
static_assert(AuditInterfaceABI<ILogicWorld>());
namespace {
struct LogicComponent {
    std::string_view name;
    uint32_t         size, alignment;
    uint64_t         hash;
    uint32_t (*family)() noexcept;
    void* (*ensure)(ECS::Registry&, Entity) noexcept;
    void (*remove)(ECS::Registry&, Entity) noexcept;
};
template <class T>
consteval auto PublishLogic() -> LogicComponent {
    return {
        Reflect::CanonicalTypeName<T>(),
        sizeof(T),
        alignof(T),
        Reflect::LayoutHash<T>(),
        +[]() noexcept { return ECS::ComponentFamily::GetTypeID<T>(); },
        +[](ECS::Registry& r, Entity e) noexcept -> void* {
            if (auto p = r.Get<T>(e))
                return &*p;
            return &r.Add<T>(e);
        },
        +[](ECS::Registry& r, Entity e) noexcept { r.Remove<T>(e); }
    };
}
constexpr std::array kLogicComponents {
    PublishLogic<Components::PendingDestroy>(),
    PublishLogic<Components::TransformComponent>(),
    PublishLogic<Components::WorldTransformComponent>(),
    PublishLogic<Components::HierarchyComponent>(),
    PublishLogic<Components::NameComponent>(),
    PublishLogic<Components::PrefabInstanceComponent>(),
    PublishLogic<Components::SceneSourceComponent>(),
    PublishLogic<Components::PBRComponent>(),
    PublishLogic<Components::AmbientCubeComponent>(),
    PublishLogic<Components::MeshComponent>(),
    PublishLogic<Components::SkeletalMeshComponent>(),
    PublishLogic<Components::MorphTargetComponent>(),
    PublishLogic<Components::LODComponent>(),
    PublishLogic<Components::CameraComponent>(),
    PublishLogic<Components::MainCameraTagComponent>(),
    PublishLogic<Components::LightComponent>(),
    PublishLogic<Components::SceneLightTagComponent>(),
    PublishLogic<Components::SunTagComponent>(),
    PublishLogic<Components::CharacterMotorComponent>(),
    PublishLogic<Components::CharacterMotorStateComponent>(),
    PublishLogic<Components::PhysicsComponent>(),
    PublishLogic<Components::ImpulseCommand>(),
    PublishLogic<Components::RagdollComponent>(),
    PublishLogic<Components::RagdollHitReactionCommand>(),
    PublishLogic<Components::RagdollImpulseCommand>(),
    PublishLogic<Components::AudioListenerComponent>(),
    PublishLogic<Components::AudioSourceComponent>(),
    PublishLogic<Components::PlayerTagComponent>(),
    PublishLogic<Components::PlayerSeatComponent>(),
};
auto                 FindLogic(StringView name) noexcept -> const LogicComponent* {
    if (!name.data && name.size)
        return nullptr;
    auto text = name.size ? std::string_view(name) : std::string_view {};
    for (const auto& c: kLogicComponents)
        if (c.name == text)
            return &c;
    return nullptr;
}
} // namespace
class LogicWorld final: public Implements<LogicWorld, ILogicWorld, IEntities, ISimulation> {
  public:
    LogicWorld(std::unique_ptr<World> world, PhysicsConfig config, Ref<ILogger> logger, Ref<IErrorRegistry> errors)
        : m_world(std::move(world)), m_config(config), m_logger(std::move(logger)), m_errors(std::move(errors)) {
        // Standalone worlds (no engine around them) contribute their hub as
        // the ambient sink. Spawned inside a running engine the sink is
        // already the engine's -- installing ours would silently divert
        // every ambient line (including test capture and fidelity harnesses)
        // for as long as this world lives, so leave it alone.
        if (ZHLN::GetLogSink().write == nullptr) {
            m_previousSink  = ZHLN::GetLogSink();
            ZHLN::SetLogSink(ZHLN::LogBridge::ToSink(m_logger.Get()));
            m_installedSink = true;
        }
        Reflect::ForEachNestedType<Components>([&]<class T>() {
            constexpr auto name = Reflect::CanonicalTypeName<T>();
            m_engineNames.insert(name);
            ECS::Registry::InternAndMapNameToFamilyID(name, ECS::ComponentFamily::GetTypeID<T>());
        });
    }
    [[nodiscard]] ILogger* Logger() noexcept override {
        return m_logger.Get();
    }
    [[nodiscard]] IErrorRegistry* Errors() noexcept override {
        return m_errors.Get();
    }
    IEntities* Entities() noexcept override;
    ISimulation* Simulation() noexcept override {
        return this;
    }
    Result<void> GetInfo(ChainOutNode* output) const noexcept override {
        if (!output)
            return Error::NullArgument;
        return DispatchOutChain<LogicWorldInfo>(output, [&](LogicWorldInfo& out) -> Result<void> {
            out = {m_config, m_world->GetPhysics().GetActiveBodyCount()};
            return {};
        });
    }
    Result<void> Advance(float dt) noexcept override {
        if (!std::isfinite(dt) || dt <= 0)
            return Error::InvalidArgument;
        Flush();
        Sim().Step(dt);
        return {};
    }
    void Flush() noexcept override {
        auto& r = Registry();
        ExpandPendingDestroy(r);
        auto                pending = r.GetEntitiesWith<Components::PendingDestroy>();
        std::vector<Entity> dead(pending.begin(), pending.end());
        for (auto e: dead) {
            ReleasePhysics(e);
            r.Destroy(e);
        }
    }
    void Clear() noexcept override {
        auto&                        r = Registry();
        std::vector<Physics::BodyID> bodies;
        for (const auto& p: r.GetRawArray<Components::PhysicsComponent>())
            bodies.push_back(p.physicsID);
        Sim().DestroyBodies(bodies);
        r.Clear();
    }
    Entity CreateEntity() noexcept override {
        return Registry().Create();
    }
    void Despawn(Entity e) noexcept override {
        if (Registry().IsAlive(e))
            MarkPendingDestroy(Registry(), e);
    }
    bool IsAlive(Entity e) const noexcept override {
        return Registry().IsAlive(e);
    }
    void* AttachRaw(Entity e, StringView name, uint32_t size, uint32_t alignment) noexcept override {
        if (!name.data || !name.size || !size || !alignment || (alignment & (alignment - 1)) != 0 || !Registry().IsAlive(e) ||
            m_engineNames.contains(std::string_view(name)))
            return nullptr;
        auto family = Registry().RegisterComponentDynamic(std::string_view(name), size, alignment);
        if (family == UINT32_MAX)
            return nullptr;
        m_dynamicFamilies.insert(family);
        return Registry().AddDynamic(e, family);
    }
    bool DetachRaw(Entity e, StringView name) noexcept override {
        if (!name.data || !Registry().IsAlive(e))
            return false;
        auto id = ECS::Registry::GetFamilyIDFromName(std::string_view(name));
        return m_dynamicFamilies.contains(id) && Registry().RemoveDynamic(e, id);
    }
    void* GetRaw(Entity e, StringView name) const noexcept override {
        if (!name.data || !Registry().IsAlive(e))
            return nullptr;
        auto id = ECS::Registry::GetFamilyIDFromName(std::string_view(name));
        return m_dynamicFamilies.contains(id) ? Registry().GetRawByFamily(e, id) : nullptr;
    }
    bool IsEngineComponent(StringView name) const noexcept override {
        return FindLogic(name) != nullptr;
    }
    void* AttachEngineRaw(Entity e, StringView name, uint32_t size, uint32_t alignment, uint64_t hash) noexcept override {
        const auto* c = FindLogic(name);
        if (!c || c->size != size || c->alignment != alignment || c->hash != hash || !Registry().IsAlive(e))
            return nullptr;
        if (c->family() == ECS::ComponentFamily::GetTypeID<Components::PhysicsComponent>())
            ReleasePhysics(e);
        return c->ensure(Registry(), e);
    }
    bool DetachEngineRaw(Entity e, StringView name) noexcept override {
        const auto* c = FindLogic(name);
        if (!c || !Registry().IsAlive(e) || !Registry().GetRawByFamily(e, c->family()))
            return false;
        if (c->family() == ECS::ComponentFamily::GetTypeID<Components::PhysicsComponent>())
            ReleasePhysics(e);
        c->remove(Registry(), e);
        return true;
    }
    void* GetEngineRaw(Entity e, StringView name) const noexcept override {
        const auto* c = FindLogic(name);
        return c && Registry().IsAlive(e) ? Registry().GetRawByFamily(e, c->family()) : nullptr;
    }
    uint32_t EntitiesWithRaw(StringView name, Entity* output, uint32_t capacity) const noexcept override {
        if (!name.data)
            return 0;
        auto id = ECS::Registry::GetFamilyIDFromName(std::string_view(name));
        if (id == UINT32_MAX || (!FindLogic(name) && !m_dynamicFamilies.contains(id)))
            return 0;
        auto matching = Registry().GetEntitiesByFamilyID(id);
        if (output)
            std::copy_n(matching.begin(), std::min<size_t>(matching.size(), capacity), output);
        return static_cast<uint32_t>(matching.size());
    }
    // ------------------------------------------------------------ ISimulation
    [[nodiscard]] Physics::RaycastResult Raycast(DVec3 origin, FVec3 direction, float maxDistance, Physics::BodyID ignore) const noexcept override {
        return Sim().Raycast(origin, direction, maxDistance, ignore);
    }

    // The facade's std::vector is built and destroyed entirely inside this
    // library; what crosses is scalars into the caller's own storage.
    [[nodiscard]] uint32_t BodiesInSphereRaw(DVec3 center, float radius, Physics::BodyID* bodies, uint32_t capacity) const noexcept override {
        const std::vector<Physics::BodyID> found = Sim().OverlapSphere(center, radius);
        const auto                         total = static_cast<uint32_t>(found.size());
        if (bodies != nullptr && capacity > 0) {
            std::copy_n(found.begin(), std::min(total, capacity), bodies);
        }
        return total;
    }

    [[nodiscard]] Option<DVec3> BodyPosition(Physics::BodyID body) const noexcept override {
        const std::optional<DVec3> found = Sim().BodyPosition(body);
        if (!found.has_value())
            return std::nullopt;
        return *found;
    }

    [[nodiscard]] Option<Physics::BodyStateSnapshot> BodyState(Physics::BodyID body) const noexcept override {
        const std::optional<Physics::BodyStateSnapshot> found = Sim().BodyState(body);
        if (!found.has_value())
            return std::nullopt;
        return *found;
    }

    [[nodiscard]] Option<FVec3> BodyLinearVelocity(Physics::BodyID body) const noexcept override {
        const std::optional<FVec3> found = Sim().BodyLinearVelocity(body);
        if (!found.has_value())
            return std::nullopt;
        return *found;
    }

    [[nodiscard]] FVec3 CharacterVelocity(Physics::BodyID body) const noexcept override {
        return Sim().CharacterVelocity(body);
    }

    [[nodiscard]] bool IsCharacterOnGround(Physics::BodyID body) const noexcept override {
        return Sim().IsCharacterOnGround(body);
    }

    [[nodiscard]] bool IsBodyDynamic(Physics::BodyID body) const noexcept override {
        return Sim().IsBodyDynamic(body);
    }

    void AddImpulse(Physics::BodyID body, FVec3 impulse) noexcept override {
        Sim().AddImpulse(body, impulse);
    }

    void SetBodyLinearVelocity(Physics::BodyID body, FVec3 velocity) noexcept override {
        Sim().SetLinearVelocity(body, velocity);
    }

    void SetKinematicPose(Physics::BodyID body, DVec3 position, Quat rotation) noexcept override {
        Sim().SetKinematicPose(body, position, rotation.AsVec4());
    }

    void SetCollisionFilter(Physics::BodyID body, uint32_t category, uint32_t mask) noexcept override {
        Sim().SetCollisionFilter(body, category, mask);
    }

    void RegisterPhysicsMaterial(uint32_t materialID, float friction, float restitution) noexcept override {
        Sim().RegisterMaterial(materialID, friction, restitution);
    }

    [[nodiscard]] float CharacterGroundOffset(Physics::BodyID body) const noexcept override {
        return Sim().GetCharacterGroundOffset(body);
    }
    void SetCharacterPosition(Physics::BodyID body, DVec3 position) noexcept override {
        Sim().SetCharacterPosition(body, position);
    }
    void QueueKinematicPose(uint32_t, Span<const Mat4x4>) noexcept override {}
    void SetCharacterVelocity(Physics::BodyID body, FVec3 velocity) noexcept override {
        Sim().SetCharacterVelocity(body, velocity);
    }

    void DestroyBody(Physics::BodyID body) noexcept override {
        Sim().DestroyBody(body);
    }

    void DestroyBodies(Span<const Physics::BodyID> bodies) noexcept override {
        Sim().DestroyBodies(std::span<const Physics::BodyID>(bodies));
    }

    void DestroyRagdolls(Span<const Physics::RagdollID> ragdolls) noexcept override {
        Sim().DestroyRagdolls(std::span<const Physics::RagdollID>(ragdolls));
    }

    void ClearMeshShapeCache() noexcept override {
        Sim().ClearMeshShapeCache();
    }

    void Step(float deltaTime) noexcept override {
        Sim().Step(deltaTime);
    }

    Result<Physics::BodyID> CreateRigidBody(const ChainInNode* descChain) noexcept override {
        if (descChain == nullptr)
            return Error::NullArgument;

        Physics::RigidBodyPose               pose {};
        Physics::RigidBodySurface            surface {};
        Physics::RigidBodyFilter             filter {};
        bool                                 hasSurface     = false;
        uint32_t                             shapeNodeCount = 0;
        std::optional<Physics::ShapeVariant> builtShape;

        const Result<void> chainRes = DispatchInChain<
            Physics::Box, Physics::Sphere, Physics::Capsule, Physics::Cylinder, Physics::Plane, Physics::TriangleMesh,
            Physics::MeshAsset, Physics::HeightField, Physics::Compound, Physics::RigidBodyPose,
            Physics::RigidBodySurface, Physics::RigidBodyFilter>(descChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
            if constexpr (std::is_same_v<NodeT, Physics::RigidBodyPose>) {
                pose = node;
            } else if constexpr (std::is_same_v<NodeT, Physics::RigidBodySurface>) {
                surface    = node;
                hasSurface = true;
            } else if constexpr (std::is_same_v<NodeT, Physics::RigidBodyFilter>) {
                filter = node;
            } else {
                ++shapeNodeCount;
                if constexpr (std::is_same_v<NodeT, Physics::Box>) {
                    if (node.halfExtents.x <= 0.0F || node.halfExtents.y <= 0.0F || node.halfExtents.z <= 0.0F) {
                        return Error::InvalidArgument;
                    }
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::Sphere>) {
                    if (node.radius <= 0.0F)
                        return Error::InvalidArgument;
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::Capsule>) {
                    if (node.halfHeight <= 0.0F || node.radius <= 0.0F)
                        return Error::InvalidArgument;
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::Cylinder>) {
                    if (node.halfHeight <= 0.0F || node.radius <= 0.0F)
                        return Error::InvalidArgument;
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::Plane>) {
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::TriangleMesh>) {
                    if (node.vertices.size() < 3U || node.indices.size() < 3U || (node.indices.size() % 3U) != 0U) {
                        return Error::InvalidArgument;
                    }
                    for (const uint32_t index: node.indices) {
                        if (index >= node.vertices.size())
                            return Error::InvalidArgument;
                    }
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::MeshAsset>) {
                    // A standalone logic world binds no asset resolver, so a
                    // cached-collider node can never resolve here.
                    return Error::AssetNotCached;
                } else if constexpr (std::is_same_v<NodeT, Physics::HeightField>) {
                    if (node.sampleCount < 2U || node.worldSize <= 0.0F) {
                        return Error::InvalidArgument;
                    }
                    const size_t totalSamples = static_cast<size_t>(node.sampleCount) * node.sampleCount;
                    if (node.heights.size() < totalSamples) {
                        return Error::InvalidArgument;
                    }
                    builtShape = node;
                } else if constexpr (std::is_same_v<NodeT, Physics::Compound>) {
                    const size_t childCount = node.boxes.size() + node.spheres.size() + node.capsules.size() +
                                              node.cylinders.size() + node.planes.size() + node.meshes.size() +
                                              node.meshAssets.size();
                    if (childCount == 0U)
                        return Error::InvalidArgument;
                    for (const auto& child: node.boxes)
                        if (child.shape.halfExtents.x <= 0.0F || child.shape.halfExtents.y <= 0.0F ||
                            child.shape.halfExtents.z <= 0.0F)
                            return Error::InvalidArgument;
                    for (const auto& child: node.spheres)
                        if (child.shape.radius <= 0.0F)
                            return Error::InvalidArgument;
                    for (const auto& child: node.capsules)
                        if (child.shape.halfHeight <= 0.0F || child.shape.radius <= 0.0F)
                            return Error::InvalidArgument;
                    for (const auto& child: node.cylinders)
                        if (child.shape.halfHeight <= 0.0F || child.shape.radius <= 0.0F)
                            return Error::InvalidArgument;
                    builtShape = node;
                }
            }
            return {};
        });
        if (!chainRes.HasValue())
            return chainRes.error;
        if (shapeNodeCount != 1U || !builtShape.has_value())
            return Error::InvalidArgument;

        if (hasSurface && surface.registerMaterial) {
            Sim().RegisterMaterial(surface.materialID, surface.friction, surface.restitution);
        }

        const Physics::ObjectLayer resolvedLayer =
            filter.hasExplicitLayer ? filter.layer :
                                      (pose.motionType == Physics::MotionType::Static ? Physics::ObjectLayer::NonMoving
                                                                                      : Physics::ObjectLayer::Moving);

        return std::visit(
            [&]<typename S>(const S& shape) -> Result<Physics::BodyID> {
                return Sim().CreateRigidBody(
                    shape, pose.position, pose.rotation.AsVec4(), pose.motionType, resolvedLayer, surface.materialID,
                    filter.category, filter.mask, pose.activation, pose.scale
                );
            },
            *builtShape
        );
    }

    Result<Physics::BodyID> CreateCharacter(const ChainInNode* descChain) noexcept override {
        Physics::CharacterCreateDesc            desc {};
        std::optional<Physics::DualShape> dual {};
        if (descChain != nullptr) {
            const Result<void> chainRes = DispatchInChain<Physics::CharacterCreateDesc, Physics::DualShape>(
                descChain, [&]<typename Node>(const Node& node) -> Result<void> {
                    if constexpr (std::is_same_v<Node, Physics::CharacterCreateDesc>) {
                        desc = node;
                    } else {
                        dual = node;
                    }
                    return {};
                }
            );
            if (!chainRes.HasValue())
                return chainRes.error;
        }
        if (dual.has_value()) {
            if (dual->lifterRadius <= 0.0F || dual->bumperRadiusXZ <= 0.0F || dual->bumperRadiusY <= 0.0F) {
                return Error::InvalidArgument;
            }
            const Physics::CharacterParams params {
                .shape                    = Physics::CreateDualShape(*dual),
                .maxSlopeAngle            = desc.maxSlopeRadians,
                .maxStrength              = desc.maxStrength,
                .characterPadding         = desc.characterPadding,
                .penetrationRecoverySpeed = desc.penetrationRecoverySpeed,
                .supportingVolume         = JPH::Plane(JPH::Vec3::sAxisY(), -dual->GetLifterOffsetY()),
                .category                 = desc.category,
                .mask                     = desc.mask,
            };
            return Sim().CreateCharacter(desc.position, params);
        }
        if (desc.useCustomCapsule && (desc.capsuleRadius <= 0.0F || desc.capsuleHalfHeight <= 0.0F)) {
            return Error::InvalidArgument;
        }
        return Sim().CreateCustomCharacter(desc);
    }

    [[nodiscard]] Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept override {
        return Sim().GetDebugDrawData(shapes, constraints, wireframe);
    }

  private:
    friend class Implements<LogicWorld, ILogicWorld, IEntities, ISimulation>;
    ~LogicWorld() {
        // Restore the sink that predates this world (usually a session's
        // hub bridge or the test capture sink) before m_logger is released.
        if (m_installedSink) {
            ZHLN::SetLogSink(m_previousSink);
        }
    }
    auto Registry() const noexcept -> ECS::Registry& {
        return m_world->GetRegistry();
    }
    auto Sim() const noexcept -> PhysicsContext& {
        return m_world->GetPhysics();
    }
    void ReleasePhysics(Entity e) noexcept {
        if (auto p = Registry().Get<Components::PhysicsComponent>(e)) {
            Sim().DestroyBody(std::exchange(p->physicsID, Physics::BodyID::Null()));
        }
    }
    std::unique_ptr<World>               m_world;
    PhysicsConfig                        m_config;
    Ref<ILogger>                         m_logger;
    ZHLN::LogSink                        m_previousSink {};
    bool                                 m_installedSink = false;
    Ref<IErrorRegistry>                  m_errors;
    std::unordered_set<uint32_t>         m_dynamicFamilies;
    std::unordered_set<std::string_view> m_engineNames;
};

IEntities* LogicWorld::Entities() noexcept {
    return this;
}
auto CreateLogicWorldRaw(const ChainInNode* config) noexcept -> Result<ILogicWorld*> {
    PhysicsConfig physics {};
    LoggerConfig  loggerNode {};
    ErrorConfig   errorNode {};
    // One dispatch over every node this factory accepts. Two sequential
    // DispatchInChain calls could never both succeed: each rejects any node in
    // the chain that is not in its own AllowedTs list, so a chain carrying
    // PhysicsConfig failed the LoggerConfig/ErrorConfig pass and a chain
    // carrying either of those failed the PhysicsConfig pass. Only a null chain
    // got through.
    const auto read = DispatchInChain<PhysicsConfig, LoggerConfig, ErrorConfig>(
        config, [&]<typename Node>(const Node& node) -> Result<void> {
            if constexpr (std::is_same_v<Node, PhysicsConfig>) {
                physics = node;
            } else if constexpr (std::is_same_v<Node, LoggerConfig>) {
                loggerNode = node;
            } else {
                errorNode = node;
            }
            return {};
        }
    );
    if (!read.HasValue())
        return read.error;
    if (!physics.maxBodies || !physics.maxBodyPairs || !physics.maxContactConstraints || !physics.tempAllocatorSize)
        return Error::InvalidArgument;
    Ref<ILogger>        logger;
    Ref<IErrorRegistry> errors(errorNode.registry);
    if (loggerNode.logger != nullptr) {
        logger = Ref<ILogger>(loggerNode.logger);
    } else {
        // Honor the level the caller asked for: a world created without a
        // LoggerConfig node still gets the console default, but one that named
        // a level gets that level instead of the factory's Info.
        auto made = CreateLogger(InChain(LoggerConfig {.logger = nullptr, .level = loggerNode.level}));
        if (!made) return made.error();
        logger = Ref<ILogger>(made->Get());
    }
    if (errors.Get() == nullptr) {
        // A caller-supplied registry (above) already carries whatever detail it
        // recorded; this branch only runs when there is nothing to seed from.
        auto made = CreateErrorRegistry();
        if (!made) return made.error();
        errors = std::move(*made);
    }
    auto world = World::Create(physics, false);
    if (!world)
        return world.error();
    auto* object = new (std::nothrow) LogicWorld(std::move(*world), physics, std::move(logger), std::move(errors));
    if (!object)
        return Error::AllocationFailed;
    return static_cast<ILogicWorld*>(object);
}
} // namespace ZHLN::COM
