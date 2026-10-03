// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestLuaDataModel.cpp
//
// Exercises the DataModel adapter through the repository's existing LuaJIT
// runtime: userdata identity, tree operations, value types, typed properties,
// and main-thread event callbacks all round-trip to the same C++ Instance graph.

#include "TestsFramework.hpp"

#include <DataModel/DataModel.hpp>
#include <Scripting/Lua/DataModelBindings.hpp>
#include <Scripting/Lua/LuaScriptRuntime.hpp>

#include <Jolt/Math/Vec3.h>
#include <cmath>
#include <expected>
#include <string>
#include <string_view>

namespace {
using namespace ZHLN::ProjectLight;
}

struct TestLuaDataModelSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> binds_existing_luajit_runtime_to_the_canonical_instance_graph() {
            DataModel dataModel;
            auto workspaceResult = dataModel.Create<WorkspaceService>("Workspace");
            auto playersResult = dataModel.Create<PlayersService>("Players");
            auto playerResult = dataModel.Create<Player>("Builder");
            ZHLN::Test::ExpectTrue(workspaceResult && playersResult && playerResult);
            if (!workspaceResult || !playersResult || !playerResult) return {};

            auto workspace = *workspaceResult;
            auto players = *playersResult;
            auto player = *playerResult;
            ZHLN::Test::ExpectTrue(workspace->SetParent(dataModel.Root()).has_value());
            ZHLN::Test::ExpectTrue(players->SetParent(dataModel.Root()).has_value());
            player->SetUserId(731);
            ZHLN::Test::ExpectTrue(player->SetParent(players).has_value());
            ZHLN::Test::ExpectTrue(players->SetLocalPlayer(player).has_value());

            ZHLN::LuaScriptRuntime runtime;
            runtime.AddBindingInitializer([&dataModel](lua_State* state) {
                RegisterDataModelLuaBindings(state, dataModel);
            });
            runtime.Initialize(nullptr);
            runtime.ExecuteString(R"lua(
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
            runtime.Shutdown();

            const InstancePtr characterInstance = workspace->FindFirstChild("Builder");
            ZHLN::Test::ExpectTrue(characterInstance != nullptr);
            if (!characterInstance) return {};
            ZHLN::Test::ExpectTrue(characterInstance->IsA("Model"));
            const auto character = StaticRefCast<Model>(characterInstance);
            const auto torsoInstance = character->FindFirstChild("Torso");
            ZHLN::Test::ExpectTrue(torsoInstance && torsoInstance->IsA("Part"));
            if (!torsoInstance || !torsoInstance->IsA("Part")) return {};
            const auto torso = StaticRefCast<Part>(torsoInstance);

            ZHLN::Test::ExpectTrue(torso->Anchored());
            ZHLN::Test::ExpectTrue(torso->CanCollide());
            ZHLN::Test::ExpectTrue(torso->Position().GetX() == 1.0f && torso->Position().GetY() == 3.0f && torso->Position().GetZ() == 3.0f);
            ZHLN::Test::ExpectTrue(torso->Size().GetX() == 2.0f && torso->Size().GetY() == 2.0f && torso->Size().GetZ() == 1.0f);
            ZHLN::Test::ExpectTrue(torso->Color().GetX() == 1.0f && std::abs(torso->Color().GetY() - (128.0f / 255.0f)) < 0.0001f);
            ZHLN::Test::ExpectTrue(torso->Shape() == PartShape::Head);
            ZHLN::Test::ExpectTrue(torso->TopSurface() == PartSurface::Smooth);
            ZHLN::Test::ExpectTrue(character->PrimaryPart() == torso);
            ZHLN::Test::ExpectTrue(player->Character() == character);

            const auto humanoidInstance = character->FindFirstChild("Humanoid");
            const auto decalInstance = torso->FindFirstChild("Logo");
            const auto soundInstance = torso->FindFirstChild("WalkSound");
            const auto motorInstance = torso->FindFirstChild("TorsoMotor");
            const auto spawnInstance = workspace->FindFirstChild("Spawn");
            ZHLN::Test::ExpectTrue(humanoidInstance && humanoidInstance->IsA("Humanoid"));
            ZHLN::Test::ExpectTrue(decalInstance && decalInstance->IsA("Decal"));
            ZHLN::Test::ExpectTrue(soundInstance && soundInstance->IsA("Sound"));
            ZHLN::Test::ExpectTrue(motorInstance && motorInstance->IsA("Motor"));
            ZHLN::Test::ExpectTrue(spawnInstance && spawnInstance->IsA("SpawnPoint"));
            if (!humanoidInstance || !decalInstance || !soundInstance || !motorInstance || !spawnInstance) return {};

            const auto humanoid = StaticRefCast<Humanoid>(humanoidInstance);
            const auto decal = StaticRefCast<Decal>(decalInstance);
            const auto sound = StaticRefCast<Sound>(soundInstance);
            const auto motor = StaticRefCast<Motor>(motorInstance);
            const auto spawn = StaticRefCast<SpawnPoint>(spawnInstance);
            ZHLN::Test::ExpectTrue(humanoid->RootPart() == torso);
            ZHLN::Test::ExpectTrue(humanoid->State() == HumanoidState::Running);
            ZHLN::Test::ExpectEq(decal->TextureId(), std::string("images/logo_tshirt.png"));
            ZHLN::Test::ExpectTrue(decal->Face() == DecalFace::Front && decal->WrapMode() == DecalWrapMode::Stretch);
            ZHLN::Test::ExpectTrue(sound->Playing() && sound->Loops() && sound->Volume() == 0.75f);
            ZHLN::Test::ExpectTrue(motor->Part1() == torso && motor->Part2() == torso);
            ZHLN::Test::ExpectTrue(motor->Offset1().GetY() == 0.5f && motor->MaxVelocity() == 12.0f);
            ZHLN::Test::ExpectTrue(spawn->Position().GetY() == 100.0f && spawn->Rotation().GetY() == 45.0f);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TestLuaDataModelSuite>();
}
