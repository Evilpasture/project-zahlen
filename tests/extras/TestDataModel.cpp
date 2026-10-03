// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestDataModel.cpp
//
// Exercises the C++ DataModel OOP wrappers: stable identity, service lookup,
// tree operations, typed wrappers, reference validation and destruction.

#include "TestsFramework.hpp"

#include <DataModel/DataModel.hpp>
#include <Jolt/Math/Vec3.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ZHLN::ProjectLight;

} // namespace

struct TestDataModelSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> preserves_explicit_ids_and_known_classes() {
            DataModel model;
            auto root = model.CreateInstanceWithId(99, "DataModel", "game");
            auto workspace = model.CreateInstanceWithId(100, "WorkspaceService");
            auto players = model.CreateInstanceWithId(101, "PlayersService");
            auto runService = model.CreateInstanceWithId(102, "RunService");
            auto soundService = model.CreateInstanceWithId(103, "SoundService");
            auto baseplate = model.CreateInstanceWithId(5001, "Part", "Baseplate");
            auto unknown = model.CreateInstanceWithId(5002, "FutureService", "Future");
            auto soundInstance = model.CreateInstanceWithId(5003, "Sound", "Click");
            auto duplicate = model.CreateInstanceWithId(100, "Folder", "Duplicate");

            ZHLN::Test::ExpectTrue(root.has_value());
            ZHLN::Test::ExpectTrue(workspace.has_value());
            ZHLN::Test::ExpectTrue(players.has_value());
            ZHLN::Test::ExpectTrue(runService.has_value());
            ZHLN::Test::ExpectTrue(soundService.has_value());
            ZHLN::Test::ExpectTrue(baseplate.has_value());
            ZHLN::Test::ExpectTrue(unknown.has_value());
            ZHLN::Test::ExpectTrue(soundInstance.has_value());
            ZHLN::Test::ExpectTrue(!duplicate.has_value());
            if (!root || !workspace || !players || !runService || !soundService || !baseplate || !unknown || !soundInstance) return {};

            ZHLN::Test::ExpectTrue(*root == model.Root());
            ZHLN::Test::ExpectTrue(model.FindById(99) == model.Root());
            ZHLN::Test::ExpectTrue((*workspace)->IsA("Service"));
            ZHLN::Test::ExpectTrue((*workspace)->IsA("Workspace"));
            ZHLN::Test::ExpectEq((*workspace)->Name(), std::string("Workspace"));
            ZHLN::Test::ExpectEq((*baseplate)->ClassName(), std::string_view("Part"));
            ZHLN::Test::ExpectTrue((*baseplate)->IsA("BasePart"));
            ZHLN::Test::ExpectTrue((*baseplate)->IsA("Instance"));
            ZHLN::Test::ExpectEq((*unknown)->ClassName(), std::string_view("FutureService"));
            ZHLN::Test::ExpectTrue((*unknown)->IsA("FutureService"));
            ZHLN::Test::ExpectEq((*soundService)->Name(), std::string("SoundService"));
            ZHLN::Test::ExpectTrue((*soundService)->IsService());
            ZHLN::Test::ExpectEq((*soundInstance)->ClassName(), std::string_view("Sound"));
            ZHLN::Test::ExpectTrue(!(*soundInstance)->IsService());
            ZHLN::Test::ExpectTrue(model.FindById(5001) == *baseplate);

            ZHLN::Test::ExpectTrue((*workspace)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*players)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*runService)->IsService());
            ZHLN::Test::ExpectTrue((*runService)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*soundService)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue(model.GetService<WorkspaceService>("Workspace") == *workspace);
            ZHLN::Test::ExpectTrue(model.GetService<PlayersService>("Players") == *players);
            ZHLN::Test::ExpectTrue(model.GetService<SoundService>("SoundService") == *soundService);
            ZHLN::Test::ExpectTrue(model.GetService<Service>("RunService") == model.GetService("RunService"));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> maintains_parentage_signals_and_rejects_cycles() {
            DataModel model;
            auto workspace = model.Create<WorkspaceService>("Workspace");
            auto folder = model.Create<Folder>("Characters");
            auto part = model.Create<Part>("RootPart");
            ZHLN::Test::ExpectTrue(workspace.has_value());
            ZHLN::Test::ExpectTrue(folder.has_value());
            ZHLN::Test::ExpectTrue(part.has_value());
            if (!workspace || !folder || !part) return {};

            ZHLN::Test::ExpectTrue((*workspace)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*folder)->SetParent(*workspace).has_value());
            ZHLN::Test::ExpectTrue((*part)->SetParent(*folder).has_value());

            size_t childAdded = 0;
            size_t childRemoved = 0;
            size_t descendantAdded = 0;
            size_t descendantRemoved = 0;
            auto addedConnection = (*workspace)->ChildAdded.Connect([&](InstancePtr) { ++childAdded; });
            auto removedConnection = (*workspace)->ChildRemoved.Connect([&](InstancePtr) { ++childRemoved; });
            auto descendantAddedConnection = (*workspace)->DescendantAdded.Connect([&](InstancePtr) { ++descendantAdded; });
            auto descendantRemovedConnection = (*workspace)->DescendantRemoved.Connect([&](InstancePtr) { ++descendantRemoved; });
            auto nested = model.Create<Folder>("Nested");
            ZHLN::Test::ExpectTrue(nested.has_value());
            if (nested) ZHLN::Test::ExpectTrue((*nested)->SetParent(*workspace).has_value());

            ZHLN::Test::ExpectEq(childAdded, size_t {1});
            ZHLN::Test::ExpectEq(descendantAdded, size_t {1});
            ZHLN::Test::ExpectEq((*part)->GetAncestors().size(), size_t {3});
            ZHLN::Test::ExpectTrue((*part)->Parent() == *folder);
            ZHLN::Test::ExpectTrue((*part)->IsDescendantOf(*workspace));
            ZHLN::Test::ExpectTrue((*workspace)->FindFirstChild("RootPart", true) == *part);
            ZHLN::Test::ExpectTrue((*workspace)->FindFirstChildWhichIsA("BasePart", true) == *part);
            ZHLN::Test::ExpectTrue(!(*folder)->SetParent(*part).has_value());
            ZHLN::Test::ExpectEq((*part)->GetFullName(), std::string("game.Workspace.Characters.RootPart"));

            (*folder)->Destroy();
            ZHLN::Test::ExpectTrue((*folder)->IsDestroyed());
            ZHLN::Test::ExpectTrue((*part)->IsDestroyed());
            ZHLN::Test::ExpectTrue(!model.FindById((*part)->Id()));
            const auto reusedId = model.CreateWithId<Folder>((*part)->Id());
            ZHLN::Test::ExpectTrue(!reusedId.has_value());
            ZHLN::Test::ExpectEq(childRemoved, size_t {1});
            ZHLN::Test::ExpectEq(descendantRemoved, size_t {2});
            ZHLN::Test::ExpectTrue(addedConnection.IsConnected());
            ZHLN::Test::ExpectTrue(removedConnection.IsConnected());
            ZHLN::Test::ExpectTrue(descendantAddedConnection.IsConnected());
            ZHLN::Test::ExpectTrue(descendantRemovedConnection.IsConnected());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> internal_stores_grow_and_preserve_names_and_snapshot_api() {
            DataModel model;
            auto workspace = model.Create<WorkspaceService>("Workspace");
            ZHLN::Test::ExpectTrue(workspace.has_value());
            if (!workspace) return {};
            ZHLN::Test::ExpectTrue((*workspace)->SetParent(model.Root()).has_value());

            std::vector<InstancePtr> created;
            created.reserve(40);
            for (size_t i = 0; i < 40; ++i) {
                auto child = model.Create<Folder>("Folder_" + std::to_string(i));
                ZHLN::Test::ExpectTrue(child.has_value());
                if (!child) return {};
                ZHLN::Test::ExpectTrue((*child)->SetParent(*workspace).has_value());
                created.push_back(*child);
            }

            // The public snapshot type stays std::vector for API compatibility,
            // while private tree storage and the ID registry grow internally.
            const std::vector<InstancePtr> snapshot = (*workspace)->GetChildren();
            ZHLN::Test::ExpectEq(snapshot.size(), size_t {40});
            for (size_t i = 0; i < created.size(); ++i) {
                ZHLN::Test::ExpectTrue(snapshot[i] == created[i]);
                ZHLN::Test::ExpectTrue(model.FindById(created[i]->Id()) == created[i]);
            }

            const InstanceId reservedId = created[17]->Id();
            created[17]->Destroy();
            ZHLN::Test::ExpectTrue(!model.FindById(reservedId));
            ZHLN::Test::ExpectTrue(!model.CreateWithId<Folder>(reservedId).has_value());
            ZHLN::Test::ExpectEq(snapshot.size(), size_t {40});

            const std::string arbitraryName = "import/" + std::string(180, 'N') + ".mesh::body";
            auto longNamed = model.Create<Folder>(arbitraryName);
            ZHLN::Test::ExpectTrue(longNamed.has_value());
            if (!longNamed) return {};
            ZHLN::Test::ExpectEq((*longNamed)->Name(), arbitraryName);
            ZHLN::Test::ExpectTrue((*longNamed)->SetParent(*workspace).has_value());
            ZHLN::Test::ExpectEq((*longNamed)->GetFullName(), std::string("game.Workspace.") + arbitraryName);
            ZHLN::Test::ExpectTrue((*workspace)->FindFirstChild(arbitraryName) == *longNamed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> signal_slots_grow_and_disconnect_independently() {
            DataModel model;
            auto workspace = model.Create<WorkspaceService>("Workspace");
            ZHLN::Test::ExpectTrue(workspace.has_value());
            if (!workspace) return {};
            ZHLN::Test::ExpectTrue((*workspace)->SetParent(model.Root()).has_value());

            size_t firstCalls = 0;
            size_t secondCalls = 0;
            size_t thirdCalls = 0;
            auto first = (*workspace)->ChildAdded.Connect([&](InstancePtr) { ++firstCalls; });
            auto second = (*workspace)->ChildAdded.Connect([&](InstancePtr) { ++secondCalls; });
            auto third = (*workspace)->ChildAdded.Connect([&](InstancePtr) { ++thirdCalls; });
            auto firstChild = model.Create<Folder>("First");
            ZHLN::Test::ExpectTrue(firstChild.has_value());
            if (!firstChild) return {};
            ZHLN::Test::ExpectTrue((*firstChild)->SetParent(*workspace).has_value());
            ZHLN::Test::ExpectEq(firstCalls, size_t {1});
            ZHLN::Test::ExpectEq(secondCalls, size_t {1});
            ZHLN::Test::ExpectEq(thirdCalls, size_t {1});

            second.Disconnect();
            ZHLN::Test::ExpectTrue(!second.IsConnected());
            auto secondChild = model.Create<Folder>("Second");
            ZHLN::Test::ExpectTrue(secondChild.has_value());
            if (!secondChild) return {};
            ZHLN::Test::ExpectTrue((*secondChild)->SetParent(*workspace).has_value());
            ZHLN::Test::ExpectEq(firstCalls, size_t {2});
            ZHLN::Test::ExpectEq(secondCalls, size_t {1});
            ZHLN::Test::ExpectEq(thirdCalls, size_t {2});
            ZHLN::Test::ExpectTrue(first.IsConnected() && third.IsConnected());
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> typed_properties_references_and_ecs_binding() {
            DataModel model;
            DataModel otherModel;
            auto workspace = model.Create<WorkspaceService>("Workspace");
            auto players = model.Create<PlayersService>("Players");
            auto modelInstance = model.Create<Model>("Avatar");
            auto rootPart = model.Create<Part>("HumanoidRootPart");
            auto humanoid = model.Create<Humanoid>("Humanoid");
            auto player = model.Create<Player>("PlayerEight");
            auto foreignPart = otherModel.Create<Part>("ForeignPart");
            ZHLN::Test::ExpectTrue(workspace && players && modelInstance && rootPart && humanoid && player && foreignPart);
            if (!workspace || !players || !modelInstance || !rootPart || !humanoid || !player || !foreignPart) return {};

            ZHLN::Test::ExpectTrue((*workspace)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*players)->SetParent(model.Root()).has_value());
            ZHLN::Test::ExpectTrue((*modelInstance)->SetParent(*workspace).has_value());
            ZHLN::Test::ExpectTrue((*rootPart)->SetParent(*modelInstance).has_value());
            ZHLN::Test::ExpectTrue((*humanoid)->SetParent(*modelInstance).has_value());
            ZHLN::Test::ExpectTrue((*player)->SetParent(*players).has_value());
            ZHLN::Test::ExpectTrue((*players)->SetLocalPlayer(*player).has_value());
            size_t propertyChangedCount = 0;
            auto propertyConnection = (*rootPart)->PropertyChanged.Connect([&](std::string) { ++propertyChangedCount; });

            (*rootPart)->SetPosition(JPH::Vec3(1.0f, 2.0f, 3.0f));
            (*rootPart)->SetSize(JPH::Vec3(2.0f, 3.0f, 4.0f));
            (*rootPart)->SetRotation(JPH::Vec3(0.0f, 90.0f, 0.0f));
            (*rootPart)->SetAnchored(true);
            ZHLN::Test::ExpectTrue((*rootPart)->SetNetworkOwner(*player).has_value());
            ZHLN::Test::ExpectTrue(!(*rootPart)->SetParent(*foreignPart).has_value());
            (*player)->SetUserId(8);
            (*humanoid)->SetWalkSpeed(18.0f);

            ZHLN::Test::ExpectTrue((*rootPart)->Position().GetY() == 2.0f);
            ZHLN::Test::ExpectTrue((*rootPart)->Size().GetZ() == 4.0f);
            ZHLN::Test::ExpectTrue((*rootPart)->Anchored());
            ZHLN::Test::ExpectTrue((*rootPart)->Rotation().GetY() == 90.0f);
            ZHLN::Test::ExpectEq(propertyChangedCount, size_t {5});
            ZHLN::Test::ExpectTrue(propertyConnection.IsConnected());
            ZHLN::Test::ExpectTrue((*rootPart)->NetworkOwner() == *player);
            ZHLN::Test::ExpectTrue((*player)->UserId() == 8);
            ZHLN::Test::ExpectTrue((*players)->FindPlayerByUserId(8) == *player);
            ZHLN::Test::ExpectEq((*players)->GetPlayers().size(), size_t {1});
            ZHLN::Test::ExpectTrue((*humanoid)->WalkSpeed() == 18.0f);
            ZHLN::Test::ExpectTrue((*modelInstance)->SetPrimaryPart(*rootPart).has_value());
            ZHLN::Test::ExpectTrue((*humanoid)->SetRootPart(*rootPart).has_value());
            ZHLN::Test::ExpectTrue((*player)->SetCharacter(*modelInstance).has_value());
            ZHLN::Test::ExpectTrue(!(*humanoid)->SetRootPart(*foreignPart).has_value());

            const ZHLN::Entity entity {.index = 42, .generation = 7};
            (*rootPart)->BindEntity(entity);
            ZHLN::Test::ExpectTrue((*rootPart)->BackingEntity() == entity);
            (*rootPart)->UnbindEntity();
            ZHLN::Test::ExpectTrue(!(*rootPart)->BackingEntity().has_value());
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TestDataModelSuite>();
}
