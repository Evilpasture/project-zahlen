// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// Exercises the DataModel <-> Lua::VirtualMachine adapter:
//   * Zero-co-ownership POD `InstanceId` userdata identity, equality, and weak
//     table interning (`rawequal` and table-key identity).
//   * Tree operations, Vector3/Color3 value types, reflected enums, and typed
//     properties round-tripping to the canonical C++ `DataModel` graph.
//   * Safe tombstone behavior after `Instance::Destroy()` (no dangling pointers,
//     no ID reuse, `IsDestroyed == true`, `tostring == "Instance<expired>"`).
//   * Signal `Connect`/`Disconnect`, session teardown lifetime decoupling
//     (destroying `VirtualMachine` + `DataModelLuaSession` before `DataModel`, or destroying
//     `DataModelLuaSession` before `VirtualMachine`), and budgeted callback protection.

#include "TestsFramework.hpp"

#include <DataModel/DataModel.hpp>
#include <LuaBindings/DataModelBindings.hpp>
#include <Zahlen/Error.hpp>
#include <lua/Lua.hpp>

#include <cmath>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace {
using namespace ZHLN::ProjectLight;
}

namespace {

struct TestLuaDataModelSuite {
    struct Tests {
        auto binds_existing_luajit_runtime_to_the_canonical_instance_graph()
            -> std::expected<void, ZHLN::ErrorCode> {
            DataModel dataModel;
            auto      workspaceResult = dataModel.Create<WorkspaceService>("Workspace");
            auto      playersResult   = dataModel.Create<PlayersService>("Players");
            auto      playerResult    = dataModel.Create<Player>("Builder");
            ZHLN::Test::ExpectTrue(workspaceResult && playersResult && playerResult);
            if (!workspaceResult || !playersResult || !playerResult) {
                return {};
            }

            auto workspace = *workspaceResult;
            auto players   = *playersResult;
            auto player    = *playerResult;
            ZHLN::Test::ExpectTrue(workspace->SetParent(dataModel.Root()).has_value());
            ZHLN::Test::ExpectTrue(players->SetParent(dataModel.Root()).has_value());
            player->SetUserId(731);
            ZHLN::Test::ExpectTrue(player->SetParent(players).has_value());
            ZHLN::Test::ExpectTrue(players->SetLocalPlayer(player).has_value());

            auto vm = ZHLN::Lua::VirtualMachine::Create();
            ZHLN::Test::ExpectTrue(vm.has_value());
            if (!vm) {
                return {};
            }
            auto session = DataModelLuaSession::Bind(*vm, dataModel);
            ZHLN::Test::ExpectTrue(session.has_value());
            if (!session) {
                return {};
            }

            const auto execRes = vm->ExecuteString(R"lua(
                local Workspace = game:GetService("Workspace")
                local Players = game:GetService("Players")
                local localPlayer = Players.LocalPlayer
                assert(Workspace == game.Workspace)
                assert(localPlayer.Name == "Builder" and localPlayer.UserId == 731)

                local added = {}
                Workspace.DescendantAdded:Connect(function(instance)
                    added[instance] = instance.Name
                end)

                local spawn = Instance.new("SpawnPoint", Workspace, "Spawn")
                spawn.Position = Vector3.new(0, 100, 0)
                spawn.Rotation = Vector3.new(0, 45, 0)

                local character = Instance.new("Model")
                character.Name = localPlayer.Name
                local torso = Instance.new("Part", character, "Torso")
                torso.Size = Vector3.new(2, 2, 1)
                torso.Position = Vector3.new(1, 2, 3) + Vector3.new(0, 1, 0)
                torso.Color = Color3.fromRGB(255, 128, 0)
                assert(Enum.PartShape.Block == Enum.PartShape.Cube)
                torso.Shape = Enum.PartShape.Block
                torso.Shape = "hEaD"
                torso.Shape = 7
                assert(torso.Shape == Enum.PartShape.Head)
                torso.TopSurface = Enum.PartSurface.Smooth

                local decal = Instance.new("Decal", torso, "Logo")
                decal.TextureId = "images/logo_tshirt.png"
                decal.Face = Enum.DecalFace.Front
                decal.Color = Color3.new(1, 1, 1)
                decal.WrapMode = Enum.DecalWrapMode.Stretch
                decal.Scale = 1.0

                local sound = Instance.new("Sound", torso, "WalkSound")
                sound.SoundId = "sounds/crunchywalk.wav"
                sound.Loops = true
                sound.Volume = 0.75
                sound:Play()

                local humanoid = Instance.new("Humanoid")
                humanoid.RootPart = torso
                humanoid.Parent = character
                local transitions = 0
                humanoid.StateChanged:Connect(function(oldState, newState)
                    if oldState == Enum.HumanoidState.Idle and newState == Enum.HumanoidState.Running then
                        transitions = transitions + 1
                    end
                end)

                character.PrimaryPart = torso
                local motor = Instance.new("Motor", torso, "TorsoMotor")
                motor.Part1 = torso
                motor.Part2 = torso
                motor.Offset1 = Vector3.new(0, 0.5, 0)
                motor.Offset2 = Vector3.new(0, 1, 0)
                motor.MaxVelocity = 12

                local found = character:FindFirstChild("Torso")
                localPlayer.Character = character
                character.Parent = Workspace

                local descendants = Workspace:GetDescendants()
                local sameLuaIdentity = false
                for _, descendant in ipairs(descendants) do
                    if descendant == torso and added[descendant] == "Torso" then
                        sameLuaIdentity = true
                    end
                end

                humanoid.State = Enum.HumanoidState.Running
                torso.Anchored = sameLuaIdentity
                torso.CanCollide = transitions == 1
                assert(found == torso and character:FindFirstChildWhichIsA("BasePart", true) == torso)
            )lua");
            ZHLN::Test::ExpectTrue(execRes.has_value());

            const InstancePtr characterInstance = workspace->FindFirstChild("Builder");
            ZHLN::Test::ExpectTrue(characterInstance != nullptr);
            if (!characterInstance) {
                return {};
            }
            ZHLN::Test::ExpectTrue(characterInstance->IsA("Model"));
            const auto character     = StaticRefCast<Model>(characterInstance);
            const auto torsoInstance = character->FindFirstChild("Torso");
            ZHLN::Test::ExpectTrue(torsoInstance && torsoInstance->IsA("Part"));
            if (!torsoInstance || !torsoInstance->IsA("Part")) {
                return {};
            }
            const auto torso = StaticRefCast<Part>(torsoInstance);

            ZHLN::Test::ExpectTrue(torso->Anchored());
            ZHLN::Test::ExpectTrue(torso->CanCollide());
            ZHLN::Test::ExpectTrue(torso->Position().x == 1.0f && torso->Position().y == 3.0f && torso->Position().z == 3.0f);
            ZHLN::Test::ExpectTrue(torso->Size().x == 2.0f && torso->Size().y == 2.0f && torso->Size().z == 1.0f);
            ZHLN::Test::ExpectTrue(torso->Color().x == 1.0f && std::abs(torso->Color().y - (128.0f / 255.0f)) < 0.0001f);
            ZHLN::Test::ExpectTrue(torso->Shape() == PartShape::Head);
            ZHLN::Test::ExpectTrue(torso->TopSurface() == PartSurface::Smooth);
            ZHLN::Test::ExpectTrue(character->PrimaryPart() == torso);
            ZHLN::Test::ExpectTrue(player->Character() == character);

            const auto humanoidInstance = character->FindFirstChild("Humanoid");
            const auto decalInstance    = torso->FindFirstChild("Logo");
            const auto soundInstance    = torso->FindFirstChild("WalkSound");
            const auto motorInstance    = torso->FindFirstChild("TorsoMotor");
            const auto spawnInstance    = workspace->FindFirstChild("Spawn");
            ZHLN::Test::ExpectTrue(humanoidInstance && humanoidInstance->IsA("Humanoid"));
            ZHLN::Test::ExpectTrue(decalInstance && decalInstance->IsA("Decal"));
            ZHLN::Test::ExpectTrue(soundInstance && soundInstance->IsA("Sound"));
            ZHLN::Test::ExpectTrue(motorInstance && motorInstance->IsA("Motor"));
            ZHLN::Test::ExpectTrue(spawnInstance && spawnInstance->IsA("SpawnPoint"));
            if (!humanoidInstance || !decalInstance || !soundInstance || !motorInstance || !spawnInstance) {
                return {};
            }

            const auto humanoid = StaticRefCast<Humanoid>(humanoidInstance);
            const auto decal    = StaticRefCast<Decal>(decalInstance);
            const auto sound    = StaticRefCast<Sound>(soundInstance);
            const auto motor    = StaticRefCast<Motor>(motorInstance);
            const auto spawn    = StaticRefCast<SpawnPoint>(spawnInstance);
            ZHLN::Test::ExpectTrue(humanoid->RootPart() == torso);
            ZHLN::Test::ExpectTrue(humanoid->State() == HumanoidState::Running);
            ZHLN::Test::ExpectEq(decal->GetTextureID(), std::string("images/logo_tshirt.png"));
            ZHLN::Test::ExpectTrue(decal->Face() == DecalFace::Front && decal->WrapMode() == DecalWrapMode::Stretch);
            ZHLN::Test::ExpectTrue(sound->Playing() && sound->Loops() && sound->Volume() == 0.75f);
            ZHLN::Test::ExpectTrue(motor->Part1() == torso && motor->Part2() == torso);
            ZHLN::Test::ExpectTrue(motor->Offset1().y == 0.5f && motor->MaxVelocity() == 12.0f);
            ZHLN::Test::ExpectTrue(spawn->Position().y == 100.0f && spawn->Rotation().y == 45.0f);
            return {};
        }

        auto destroyed_instances_and_disconnected_signals_never_dangle_across_heaps()
            -> std::expected<void, ZHLN::ErrorCode> {
            DataModel dataModel;
            auto      workspaceRes = dataModel.Create<WorkspaceService>("Workspace");
            ZHLN::Test::ExpectTrue(workspaceRes.has_value());
            if (!workspaceRes) {
                return {};
            }
            auto workspace = *workspaceRes;
            ZHLN::Test::ExpectTrue(workspace->SetParent(dataModel.Root()).has_value());

            auto partRes = dataModel.Create<Part>("Ephemeral");
            ZHLN::Test::ExpectTrue(partRes.has_value());
            if (!partRes) {
                return {};
            }
            Part* rawPartPtr = partRes->GetPtr();
            ZHLN::Test::ExpectTrue((*partRes)->SetParent(workspace).has_value());

            // Verify C++ refcount does NOT grow when Lua holds the Instance handle:
            // DataModel's parent tree holds 1 ref + `partRes` holds 1 ref = 2.
            const uint32_t refCountBeforeLua = rawPartPtr->GetRefCount();

            {
                ZHLN::Lua::Budget budget {.maxInstructions = 16'384, .maxMemoryBytes = 4 * 1024 * 1024};
                auto            vm = ZHLN::Lua::VirtualMachine::Create(budget);
                ZHLN::Test::ExpectTrue(vm.has_value());
                if (!vm) {
                    return {};
                }
                auto session = DataModelLuaSession::Bind(*vm, dataModel);
                ZHLN::Test::ExpectTrue(session.has_value());
                if (!session) {
                    return {};
                }

                const auto holdRes = vm->ExecuteString(R"lua(
                    heldPart = workspace:FindFirstChild("Ephemeral")
                    assert(heldPart ~= nil and heldPart.Name == "Ephemeral")
                    assert(heldPart.IsDestroyed == false)
                    propEvents = 0
                    conn = heldPart.PropertyChanged:Connect(function(prop)
                        propEvents = propEvents + 1
                    end)
                    assert(conn.Connected == true)
                )lua");
                ZHLN::Test::ExpectTrue(holdRes.has_value());

                // Crucial invariant: Lua global `heldPart` is pure POD `InstanceId`,
                // so C++ `RefTarget` refcount is completely untouched by Lua!
                ZHLN::Test::ExpectEq(rawPartPtr->GetRefCount(), refCountBeforeLua);

                (*partRes)->SetAnchored(true);
                ZHLN::Test::ExpectTrue(vm->ExecuteString("assert(propEvents == 1)").has_value());

                // Disconnect from Lua; subsequent C++ signal fires must not invoke Lua.
                ZHLN::Test::ExpectTrue(vm->ExecuteString("conn:Disconnect(); assert(conn.Connected == false)").has_value());
                (*partRes)->SetAnchored(false);
                ZHLN::Test::ExpectTrue(vm->ExecuteString("assert(propEvents == 1)").has_value());

                // Destroy the C++ Instance and release our local `partRes` strong ref
                // while Lua STILL holds `heldPart` in `_G`. C++ frees the Instance
                // immediately without waiting for Lua GC, and `heldPart` safely sees
                // the tombstone on subsequent Lua accesses.
                (*partRes)->Destroy();
                partRes = Ref<Part> {};

                const auto tombstoneCheck = vm->ExecuteString(R"lua(
                    assert(heldPart.IsDestroyed == true)
                    assert(heldPart.Name == nil)
                    assert(heldPart.Parent == nil)
                    assert(tostring(heldPart) == "Instance<expired>")
                    assert(#heldPart:GetChildren() == 0)
                    heldPart.Name = "IgnoredWrite"
                    heldPart:Destroy()

                    -- A newly created Part gets a fresh monotonic ID and never aliases `heldPart`.
                    local fresh = Instance.new("Part", workspace, "Fresh")
                    assert(fresh ~= heldPart)
                    assert(fresh.IsDestroyed == false)

                    -- Runaway callback hits the VM instruction budget cleanly instead of hanging.
                    workspace.ChildAdded:Connect(function(_)
                        while true do end
                    end)
                )lua");
                ZHLN::Test::ExpectTrue(tombstoneCheck.has_value());

                auto triggerLoopPart = dataModel.Create<Part>("TriggerBudget");
                ZHLN::Test::ExpectTrue(triggerLoopPart.has_value());
                if (triggerLoopPart) {
                    ZHLN::Test::ExpectTrue((*triggerLoopPart)->SetParent(workspace).has_value());
                }
                ZHLN::Test::ExpectTrue(session->LastCallbackError().find("instruction budget exceeded") != std::string_view::npos);
            }

            // Both `DataModelLuaSession` and `VirtualMachine` are now destroyed; firing C++
            // signals on the surviving `DataModel` must be a clean no-op.
            auto afterVmPart = dataModel.Create<Part>("AfterVmClosed");
            ZHLN::Test::ExpectTrue(afterVmPart.has_value());
            if (afterVmPart) {
                ZHLN::Test::ExpectTrue((*afterVmPart)->SetParent(workspace).has_value());
            }
            return {};
        }
    };
};

} // namespace
