// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The replication bridge without a socket and without a renderer.
//
// Everything below runs on a synthetic snapshot fed through the real parser and
// the real applier: no server, no ClientSession::Connect, no Engine. That is the
// point of the split this suite checks -- the parts of the client that decide
// what an instance *is* (its components, its uid -> entity binding, its record
// graph) can be exercised on their own, so a change to them cannot hide behind
// "needs a server to reproduce".

#include "TestsFramework.hpp"

#include <ProjectLightClient/LightProtocol.hpp>
#include <ProjectLightClient/SceneMirror.hpp>
#include <Replication/ClassVocabulary.hpp>
#include <Replication/ReplicaState.hpp>
#include <Replication/ReplicationComponents.hpp>

#include <Zahlen/GameModule.hpp>
#include <Zahlen/ecs/ECS.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

using ZHLN::ECS::BoxedName;
using ZHLN::ECS::ComponentFamily;
using ZHLN::ECS::Registry;
using ZHLN::Entity;
using ZHLN::ProjectLight::ClientSession;
using ZHLN::ProjectLight::InstanceId;
using ZHLN::ProjectLight::IsLightClass;
using ZHLN::ProjectLight::IsPartClass;
using ZHLN::ProjectLight::MsgPackValue;
using ZHLN::ProjectLight::PartShape;
using ZHLN::ProjectLight::ProjectLightInstanceComponent;
using ZHLN::ProjectLight::ProjectLightNetworkOwnerComponent;
using ZHLN::ProjectLight::ProjectLightReplicaTag;
using ZHLN::ProjectLight::ReplicationIndex;

// The uids a project-light server uses in the synthetic place below. Any
// numbers would do; naming them keeps the packet readable.
constexpr InstanceId kDataModelUid  = 1;
constexpr InstanceId kWorkspaceUid  = 2;
constexpr InstanceId kPlayersUid    = 3;
constexpr InstanceId kPlayerUid     = 10;
constexpr InstanceId kCharacterUid  = 11;
constexpr InstanceId kHumanoidUid   = 12;
constexpr InstanceId kTorsoUid      = 13;
constexpr InstanceId kHeadUid       = 14;
constexpr InstanceId kLightUid      = 20;
constexpr uint64_t   kLocalUserUid  = 4242;

MsgPackValue Instance(std::string_view className, std::string_view name, InstanceId parentUid) {
    MsgPackValue properties = MsgPackValue::Map();
    properties.Set("ClassName", MsgPackValue::String(className));
    properties.Set("Name", MsgPackValue::String(name));
    if (parentUid != 0) properties.Set("Parent", MsgPackValue::UInt(parentUid));
    return properties;
}

// The whole place in one map, in the order a real snapshot hands it over.
MsgPackValue SyntheticSnapshot() {
    MsgPackValue snapshot = MsgPackValue::Map();
    auto entry = [&](InstanceId uid, MsgPackValue properties) {
        snapshot.SetIntKey(static_cast<int64_t>(uid), std::move(properties));
    };

    entry(kDataModelUid, Instance("DataModel", "DataModel", 0));
    entry(kWorkspaceUid, Instance("WorkspaceService", "Workspace", kDataModelUid));
    entry(kPlayersUid, Instance("PlayersService", "Players", kDataModelUid));
    entry(kPlayerUid, Instance("Player", "LocalPlayer", kPlayersUid));
    entry(kCharacterUid, Instance("Character", "Character", kWorkspaceUid));
    entry(kHumanoidUid, Instance("Humanoid", "Humanoid", kCharacterUid));
    entry(kTorsoUid, Instance("Part", "Torso", kCharacterUid));

    MsgPackValue head = Instance("MeshPart", "Head", kCharacterUid);
    head.Set("Shape", MsgPackValue::Int(static_cast<int64_t>(PartShape::Sphere)));
    head.Set("Size", MsgPackValue::Array({MsgPackValue::Float(2.0f * ZHLN::ProjectLight::VEC3_SCALE),
                                          MsgPackValue::Float(2.0f * ZHLN::ProjectLight::VEC3_SCALE),
                                          MsgPackValue::Float(2.0f * ZHLN::ProjectLight::VEC3_SCALE)}));
    entry(kHeadUid, std::move(head));

    MsgPackValue light = Instance("PointLight", "Sun", kWorkspaceUid);
    light.Set("Range", MsgPackValue::Float(32.0f));
    entry(kLightUid, std::move(light));

    MsgPackValue player = MsgPackValue::Map();
    player.Set("ClassName", MsgPackValue::String("Player"));
    player.Set("Name", MsgPackValue::String("LocalPlayer"));
    player.Set("UserId", MsgPackValue::UInt(kLocalUserUid));
    player.Set("Character", MsgPackValue::UInt(kCharacterUid));
    entry(kPlayerUid, std::move(player));

    MsgPackValue character = MsgPackValue::Map();
    character.Set("ClassName", MsgPackValue::String("Character"));
    character.Set("Name", MsgPackValue::String("Character"));
    character.Set("Parent", MsgPackValue::UInt(kWorkspaceUid));
    character.Set("PrimaryPart", MsgPackValue::UInt(kTorsoUid));
    entry(kCharacterUid, std::move(character));

    return snapshot;
}

void Feed(ClientSession& session, MsgPackValue snapshot) {
    MsgPackValue packet = MsgPackValue::Map();
    packet.Set("initialDataModelSnapshot", std::move(snapshot));
    const bool handled = session.HandlePacket(packet);
    ZHLN::Test::ExpectTrue(handled);
}

} // namespace

namespace {

struct TestProjectLightReplicationSuite {
    struct Tests {
        // The module owns its components: DescribeClient names the registrar, and
        // the registrar is what maps each component's family to its name.
        std::expected<void, ZHLN::ErrorCode> the_client_module_registers_its_own_components() {
            const auto module = ZHLN::ProjectLight::DescribeClient();
            ZHLN::Test::ExpectTrue(module.registerComponents != nullptr);
            if (module.registerComponents == nullptr) return {};

            Registry registry;
            module.registerComponents(registry);

            constexpr uint32_t kUnmappedFamily = 0xFFFFFFFFu;
            ZHLN::Test::ExpectTrue(Registry::GetFamilyIDFromName(BoxedName<ProjectLightInstanceComponent>()) != kUnmappedFamily);
            ZHLN::Test::ExpectTrue(Registry::GetFamilyIDFromName(BoxedName<ProjectLightNetworkOwnerComponent>()) != kUnmappedFamily);
            ZHLN::Test::ExpectTrue(Registry::GetFamilyIDFromName(BoxedName<ProjectLightReplicaTag>()) != kUnmappedFamily);
            ZHLN::Test::ExpectEq(Registry::GetFamilyIDFromName(BoxedName<ProjectLightInstanceComponent>()), ComponentFamily::GetTypeID<ProjectLightInstanceComponent>());
            return {};
        }

        // The components an entity carries are readable back off it, which is the
        // whole reason they exist: a system holding an entity can name the
        // instance without going through the client's mirror.
        std::expected<void, ZHLN::ErrorCode> instance_identity_round_trips_through_components() {
            Registry registry;
            const Entity entity = registry.Create();
            registry.Add(
                entity,
                ProjectLightInstanceComponent {.uid = kTorsoUid},
                ProjectLightNetworkOwnerComponent {.ownerUid = kLocalUserUid},
                ProjectLightReplicaTag {}
            );

            auto instance = registry.Get<ProjectLightInstanceComponent>(entity);
            auto owner    = registry.Get<ProjectLightNetworkOwnerComponent>(entity);
            ZHLN::Test::ExpectTrue(instance.has_value());
            ZHLN::Test::ExpectTrue(owner.has_value());
            if (instance) ZHLN::Test::ExpectEq(instance->uid, InstanceId {kTorsoUid});
            if (owner) ZHLN::Test::ExpectEq(owner->ownerUid, kLocalUserUid);

            // Ownership moves without the identity moving.
            if (owner) owner->ownerUid = kLocalUserUid + 1;
            ZHLN::Test::ExpectEq(registry.Get<ProjectLightNetworkOwnerComponent>(entity)->ownerUid, kLocalUserUid + 1);
            ZHLN::Test::ExpectEq(registry.Get<ProjectLightInstanceComponent>(entity)->uid, InstanceId {kTorsoUid});
            return {};
        }

        // One binder, and it answers Null for anything it does not hold.
        std::expected<void, ZHLN::ErrorCode> replication_index_binds_takes_and_forgets() {
            ReplicationIndex index;
            ZHLN::Test::ExpectEq(index.Count(), size_t {0});
            ZHLN::Test::ExpectEq(index.Find(kTorsoUid), Entity::Null());

            Registry registry;
            const Entity torso = registry.Create();
            const Entity head  = registry.Create();
            index.Bind(kTorsoUid, torso);
            index.Bind(kHeadUid, head);
            ZHLN::Test::ExpectEq(index.Count(), size_t {2});
            ZHLN::Test::ExpectEq(index.Find(kTorsoUid), torso);
            ZHLN::Test::ExpectEq(index.Find(kHeadUid), head);

            // Take hands the entity back once, then the uid is unknown again.
            ZHLN::Test::ExpectEq(index.Take(kTorsoUid), torso);
            ZHLN::Test::ExpectEq(index.Find(kTorsoUid), Entity::Null());
            ZHLN::Test::ExpectEq(index.Take(kTorsoUid), Entity::Null());

            index.Clear();
            ZHLN::Test::ExpectTrue(index.Empty());
            ZHLN::Test::ExpectEq(index.Count(), size_t {0});
            return {};
        }

        // The parser and applier build records, parent links and class names.
        std::expected<void, ZHLN::ErrorCode> synthetic_snapshot_builds_the_mirror() {
            ClientSession session;
            Feed(session, SyntheticSnapshot());

            const auto* torso = session.FindObject(kTorsoUid);
            ZHLN::Test::ExpectTrue(torso != nullptr);
            if (torso != nullptr) {
                ZHLN::Test::ExpectEq(torso->uid, InstanceId {kTorsoUid});
                ZHLN::Test::ExpectEq(torso->className, std::string("Part"));
                ZHLN::Test::ExpectEq(torso->name, std::string("Torso"));
                ZHLN::Test::ExpectEq(torso->parentUid, uint64_t {kCharacterUid});
            }

            const auto* head = session.FindObject(kHeadUid);
            ZHLN::Test::ExpectTrue(head != nullptr);
            if (head != nullptr) {
                ZHLN::Test::ExpectEq(head->className, std::string("MeshPart"));
                ZHLN::Test::ExpectEq(head->shape, PartShape::Sphere);
            }

            const auto* player = session.FindObject(kPlayerUid);
            ZHLN::Test::ExpectTrue(player != nullptr);
            if (player != nullptr) {
                ZHLN::Test::ExpectEq(player->userId, kLocalUserUid);
                ZHLN::Test::ExpectEq(player->characterUid, uint64_t {kCharacterUid});
            }

            ZHLN::Test::ExpectEq(session.Objects().size(), size_t {9});
            ZHLN::Test::ExpectEq(session.FindObject(9999), nullptr);
            ZHLN::Test::ExpectEq(session.Index().Count(), size_t {0});
            return {};
        }

        // The record graph answers "is this part of that character" through the
        // one shared query, so the session and the scene half cannot disagree
        // about it.
        std::expected<void, ZHLN::ErrorCode> character_linkage_is_a_record_query() {
            ClientSession session;
            Feed(session, SyntheticSnapshot());

            const auto& objects = session.Objects();
            ZHLN::Test::ExpectTrue(ZHLN::ProjectLight::IsPartOfCharacter(objects, kTorsoUid, kCharacterUid));
            ZHLN::Test::ExpectTrue(ZHLN::ProjectLight::IsPartOfCharacter(objects, kHeadUid, kCharacterUid));
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::IsPartOfCharacter(objects, kLightUid, kCharacterUid));
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::IsPartOfCharacter(objects, kCharacterUid, kCharacterUid));
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::IsPartOfCharacter(objects, 0, kCharacterUid));
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::IsPartOfCharacter(objects, kTorsoUid, 0));
            ZHLN::Test::ExpectFalse(ZHLN::ProjectLight::IsLocalCharacterPart(objects, kTorsoUid, 0));
            ZHLN::Test::ExpectTrue(ZHLN::ProjectLight::IsLocalCharacterPart(objects, kTorsoUid, kCharacterUid));
            return {};
        }

        // A removal names a subtree and marks it once, however many times the
        // server says so, and the children are queued with the parent.
        std::expected<void, ZHLN::ErrorCode> tree_removals_mark_the_subtree_once() {
            ClientSession session;
            Feed(session, SyntheticSnapshot());

            MsgPackValue tree = MsgPackValue::Map();
            tree.Set("removals", MsgPackValue::Array({MsgPackValue::UInt(kCharacterUid), MsgPackValue::UInt(kCharacterUid)}));

            MsgPackValue packet = MsgPackValue::Map();
            packet.Set("treeUpdates", std::move(tree));
            ZHLN::Test::ExpectTrue(session.HandlePacket(packet));

            const auto* character = session.FindObject(kCharacterUid);
            const auto* humanoid  = session.FindObject(kHumanoidUid);
            const auto* torso     = session.FindObject(kTorsoUid);
            const auto* light     = session.FindObject(kLightUid);
            ZHLN::Test::ExpectTrue(character != nullptr && character->removalPending);
            ZHLN::Test::ExpectTrue(humanoid != nullptr && humanoid->removalPending);
            ZHLN::Test::ExpectTrue(torso != nullptr && torso->removalPending);
            // Siblings outside the subtree are untouched.
            ZHLN::Test::ExpectTrue(light != nullptr && !light->removalPending);
            return {};
        }

        // A fresh snapshot clears a pending removal for an instance that came back.
        std::expected<void, ZHLN::ErrorCode> a_later_snapshot_clears_pending_removal() {
            ClientSession session;
            Feed(session, SyntheticSnapshot());

            MsgPackValue removals = MsgPackValue::Map();
            removals.Set("removals", MsgPackValue::Array({MsgPackValue::UInt(kHeadUid)}));
            MsgPackValue removalPacket = MsgPackValue::Map();
            removalPacket.Set("treeUpdates", std::move(removals));
            ZHLN::Test::ExpectTrue(session.HandlePacket(removalPacket));
            ZHLN::Test::ExpectTrue(session.FindObject(kHeadUid)->removalPending);

            Feed(session, SyntheticSnapshot());
            ZHLN::Test::ExpectFalse(session.FindObject(kHeadUid)->removalPending);
            return {};
        }

        // The vocabulary the wire is decoded with is the same vocabulary the
        // scene half asks its questions in, and it is deliberately narrow.
        std::expected<void, ZHLN::ErrorCode> class_vocabulary_matches_the_wire() {
            ZHLN::Test::ExpectTrue(IsPartClass("Part"));
            ZHLN::Test::ExpectTrue(IsPartClass("MeshPart"));
            ZHLN::Test::ExpectFalse(IsPartClass("Model"));
            ZHLN::Test::ExpectFalse(IsPartClass(""));

            ZHLN::Test::ExpectTrue(IsLightClass("PointLight"));
            ZHLN::Test::ExpectTrue(IsLightClass("DirectionalLight"));
            // A class the light spawner would build as a point light is not
            // claimed as supported until it can be built as what it is.
            ZHLN::Test::ExpectFalse(IsLightClass("SpotLight"));

            ZHLN::Test::ExpectEq(ZHLN::ProjectLight::PartShapeFromValue(1), PartShape::Sphere);
            ZHLN::Test::ExpectEq(ZHLN::ProjectLight::PartShapeFromValue(-1), PartShape::Cube);
            ZHLN::Test::ExpectEq(ZHLN::ProjectLight::PartShapeFromValue(999), PartShape::Cube);
            ZHLN::Test::ExpectEq(ZHLN::ProjectLight::StateFromValue(5), ZHLN::ProjectLight::HumanoidState::GettingUp);
            ZHLN::Test::ExpectEq(ZHLN::ProjectLight::StateFromValue(999), ZHLN::ProjectLight::HumanoidState::Idle);
            return {};
        }

        // A record's playout history interpolates between the samples it holds and
        // holds the newest one past the end of the ring.
        std::expected<void, ZHLN::ErrorCode> transform_history_samples_between_receipts() {
            ZHLN::ProjectLight::NetworkTransformHistory history;
            ZHLN::Test::ExpectTrue(history.Empty());

            JPH::Vec3 sampledPosition {};
            JPH::Quat sampledRotation {};
            ZHLN::Test::ExpectFalse(history.Sample(std::chrono::steady_clock::now(), sampledPosition, sampledRotation));

            const auto base = std::chrono::steady_clock::now();
            history.Push({.position = JPH::Vec3(0.0f, 0.0f, 0.0f), .rotation = JPH::Quat::sIdentity(), .receivedAt = base});
            history.Push({.position = JPH::Vec3(10.0f, 0.0f, 0.0f), .rotation = JPH::Quat::sIdentity(), .receivedAt = base + std::chrono::milliseconds(10)});

            ZHLN::Test::ExpectTrue(history.Sample(base + std::chrono::milliseconds(5), sampledPosition, sampledRotation));
            ZHLN::Test::ExpectTrue(sampledPosition.GetX() > 4.0f && sampledPosition.GetX() < 6.0f);

            ZHLN::Test::ExpectTrue(history.Sample(base + std::chrono::milliseconds(50), sampledPosition, sampledRotation));
            ZHLN::Test::ExpectEq(sampledPosition.GetX(), 10.0f);
            return {};
        }
    };
};

} // namespace
