// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CharacterManager.hpp"
#include "PhysicsWorld.hpp"
#include "Zahlen/Log.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Zahlen/Physics/Physics.hpp>

namespace ZHLN::Physics {

void PhysicsWorld::ProcessCommands(std::span<Command> commands, CharacterManager& characters, TransformFrame& backFrame) {
    if (commands.empty()) {
        return;
    }

    auto& bt = bodyTable;

    for (const auto& cmd: commands) {
        switch (cmd.type) {
            case CommandType::CreateRigidBody: {
                const auto&    c    = cmd.payload.createRigid;
                const uint32_t slot = c.handle.index;
                if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != c.handle.generation) {
                    break;
                }

                const JPH::RVec3 pos(c.px, c.py, c.pz);
                const JPH::Quat  rot(c.rx, c.ry, c.rz, c.rw);

                JPH::BodyID id(c.precreatedBodyID);
                if (id.IsInvalid()) {
                    const MaterialData        mat = ResolveMaterial(c.materialID);
                    JPH::BodyCreationSettings settings(cmd.shape, pos, rot, c.motion, static_cast<JPH::ObjectLayer>(c.layer));
                    settings.mUserData    = c.handle.Pack();
                    settings.mFriction    = mat.friction;
                    settings.mRestitution = mat.restitution;

                    if (c.motion == JPH::EMotionType::Dynamic) {
                        settings.mAllowSleeping = true;
                        settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
                    }

                    id = bodyInterface->CreateAndAddBody(
                        settings, (c.motion == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate : c.activation
                    );
                }

                const auto dense      = static_cast<uint32_t>(bt.count.fetch_add(1u, std::memory_order::acq_rel));
                bt.bodyIDs[dense]     = id;
                bt.slotToDense[slot]  = dense;
                bt.denseToSlot[dense] = slot;
                bt.materialIDs[dense] = c.materialID;
                bt.userData[dense]    = c.handle.Pack();
                bt.categories[dense]  = c.category;
                bt.masks[dense]       = c.mask;

                bt.slotBodyIDs[slot].store(id.GetIndexAndSequenceNumber(), std::memory_order::release);
                bt.slotMaterialIDs[slot].store(c.materialID, std::memory_order::relaxed);

                const uint32_t jIdx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
                ZHLN::Assert(
                    jIdx < bt.idToHandleMap.size(), "PhysicsCommand: jIdx ({}) exceeds idToHandleMap size ({})!", jIdx, bt.idToHandleMap.size()
                );
                bt.idToHandleMap[jIdx].store(c.handle.Pack(), std::memory_order::release);

                backFrame.WriteInitialPose(dense, pos, rot, 0u);

                // Only transition PendingCreateRigid -> Alive if a concurrent
                // DestroyBody did not already mark this slot PendingDestroy.
                auto expected = static_cast<uint8_t>(SlotState::PendingCreateRigid);
                bt.slotStates[slot].compare_exchange_strong(
                    expected, static_cast<uint8_t>(SlotState::Alive), std::memory_order::acq_rel, std::memory_order::relaxed
                );
                break;
            }

            case CommandType::CreateCharacter: {
                const auto&    c    = cmd.payload.createChar;
                const uint32_t slot = c.handle.index;
                if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != c.handle.generation) {
                    break;
                }

                auto* character = characters.MaterializeCharacter(c, cmd.shape, *system);

                const auto dense      = static_cast<uint32_t>(bt.count.fetch_add(1u, std::memory_order::acq_rel));
                bt.bodyIDs[dense]     = JPH::BodyID();
                bt.slotToDense[slot]  = dense;
                bt.denseToSlot[dense] = slot;
                bt.materialIDs[dense] = 0u;
                bt.userData[dense]    = c.handle.Pack();
                bt.categories[dense]  = c.category;
                bt.masks[dense]       = c.mask;

                bt.slotBodyIDs[slot].store(JPH::BodyID::cInvalidBodyID, std::memory_order::release);
                bt.slotMaterialIDs[slot].store(0u, std::memory_order::relaxed);

                const uint8_t charFlags =
                    (character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) ? kCharacterFlagOnGround : 0u;
                backFrame.WriteInitialPose(dense, JPH::RVec3(c.px, c.py, c.pz), JPH::Quat::sIdentity(), charFlags);

                auto expected = static_cast<uint8_t>(SlotState::PendingCreateChar);
                bt.slotStates[slot].compare_exchange_strong(
                    expected, static_cast<uint8_t>(SlotState::Character), std::memory_order::acq_rel, std::memory_order::relaxed
                );
                break;
            }

            case CommandType::DestroyBody: {
                const uint32_t slot = cmd.payload.handle.index;
                if (slot >= bt.slotCapacity || bt.generations[slot].load(std::memory_order::acquire) != cmd.payload.handle.generation) {
                    break;
                }

                const uint32_t    dense  = bt.slotToDense[slot];
                const JPH::BodyID bodyID = bt.bodyIDs[dense];

                if (bodyID.IsInvalid()) {
                    characters.DestroyCharacterSlot(slot);
                    bt.RemoveDenseAndReleaseSlot(slot, backFrame);
                    break;
                }

                const uint32_t joltIdx = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
                ZHLN::Assert(
                    joltIdx < bt.idToHandleMap.size() && joltIdx < bt.joltBodyPtrs.size(),
                    "PhysicsCommand: joltIdx ({}) exceeds active map sizes ({}, {}) during DestroyBody!",
                    joltIdx,
                    bt.idToHandleMap.size(),
                    bt.joltBodyPtrs.size()
                );

                bt.idToHandleMap[joltIdx].store(0u, std::memory_order::release);
                bt.joltBodyPtrs[joltIdx] = nullptr;

                bodyInterface->RemoveBody(bodyID);
                bodyInterface->DestroyBody(bodyID);
                bt.RemoveDenseAndReleaseSlot(slot, backFrame);
                break;
            }

            case CommandType::SetCollisionFilter: {
                const auto&    c    = cmd.payload.setFilter;
                const uint32_t slot = c.handle.index;
                if (slot < bt.slotCapacity && bt.generations[slot].load(std::memory_order::acquire) == c.handle.generation &&
                    GetSlotPredicate(bt.LoadSlotState(slot)).isActive) {
                    const uint32_t dense = bt.slotToDense[slot];
                    bt.categories[dense] = c.category;
                    bt.masks[dense]      = c.mask;
                }
                break;
            }

            case CommandType::SetLinearVelocity: {
                const auto&       c  = cmd.payload.setVelocity;
                const JPH::BodyID id = GetBodyID(*this, c.handle);
                if (!id.IsInvalid()) {
                    bodyInterface->ActivateBody(id);
                    bodyInterface->SetLinearVelocity(id, {c.vx, c.vy, c.vz});
                }
                break;
            }

            case CommandType::SetCharacterVelocity: {
                const auto& c = cmd.payload.setVelocity;
                characters.ApplyVelocityCommand(c.handle, JPH::Vec3(c.vx, c.vy, c.vz));
                break;
            }

            case CommandType::SetCharacterPosition: {
                const auto&      c   = cmd.payload.setPosition;
                const JPH::RVec3 pos(c.px, c.py, c.pz);
                if (characters.ApplyPositionCommand(c.handle, pos) && bt.LoadSlotState(c.handle.index) == SlotState::Character) {
                    backFrame.WriteTeleportPosition(bt.slotToDense[c.handle.index], pos);
                }
                break;
            }

            case CommandType::SetKinematicPose: {
                const auto&       c  = cmd.payload.setKinematicPose;
                const JPH::BodyID id = GetBodyID(*this, c.handle);
                if (!id.IsInvalid() && bodyInterface->GetMotionType(id) != JPH::EMotionType::Static) {
                    const JPH::RVec3 pos(c.px, c.py, c.pz);
                    const JPH::Quat  rot(c.rx, c.ry, c.rz, c.rw);
                    bodyInterface->SetPositionAndRotationWhenChanged(id, pos, rot, JPH::EActivation::Activate);
                    backFrame.WriteTeleportPose(bt.slotToDense[c.handle.index], pos, rot);
                }
                break;
            }

            case CommandType::AddImpulse: {
                const auto&       c  = cmd.payload.setVelocity;
                const JPH::BodyID id = GetBodyID(*this, c.handle);
                if (!id.IsInvalid()) {
                    bodyInterface->AddImpulse(id, {c.vx, c.vy, c.vz});
                    bodyInterface->ActivateBody(id);
                }
                break;
            }

            case CommandType::AddImpulseAtPosition: {
                const auto&       c  = cmd.payload.addImpulseAtPos;
                const JPH::BodyID id = GetBodyID(*this, c.handle);
                if (!id.IsInvalid()) {
                    bodyInterface->AddImpulse(id, JPH::Vec3(c.ix, c.iy, c.iz), JPH::RVec3(c.px, c.py, c.pz));
                    bodyInterface->ActivateBody(id);
                }
                break;
            }

            case CommandType::CreateConstraint: {
                const auto&       c   = cmd.payload.createConstraint;
                const JPH::BodyID id1 = GetBodyID(*this, c.b1);
                const JPH::BodyID id2 = GetBodyID(*this, c.b2);

                JPH::BodyLockWrite lock1(system->GetBodyLockInterface(), id1);
                JPH::BodyLockWrite lock2(system->GetBodyLockInterface(), id2);

                if (lock1.Succeeded() && lock2.Succeeded()) {
                    auto* joltConstraint = CreateNativeConstraint(c.cType, &lock1.GetBody(), &lock2.GetBody(), c.params);
                    if (joltConstraint != nullptr) {
                        joltConstraint->AddRef();
                        system->AddConstraint(joltConstraint);

                        const uint32_t slot    = c.cHandle.index;
                        constraints[slot]      = joltConstraint;
                        constraintStates[slot] = SlotState::Alive;
                    }
                }
                break;
            }

            case CommandType::DestroyConstraint: {
                const uint32_t slot = cmd.payload.cHandle.index;
                if (slot < constraintCapacity && constraintGenerations[slot].load(std::memory_order::acquire) == cmd.payload.cHandle.generation) {
                    if (constraints[slot] != nullptr) {
                        system->RemoveConstraint(constraints[slot]);
                        constraints[slot]->Release();
                        constraints[slot] = nullptr;
                        RemoveConstraintSlot(slot);
                    }
                }
                break;
            }

            case CommandType::SetConstraintTarget: {
                const auto&    c    = cmd.payload.setConstraintTarget;
                const uint32_t slot = c.targetCHandle.index;
                if (slot < constraintCapacity && constraintGenerations[slot].load(std::memory_order::relaxed) == c.targetCHandle.generation) {
                    JPH::Constraint* constraint = constraints[slot];
                    if (constraint != nullptr) {
                        switch (constraint->GetSubType()) {
                            case JPH::EConstraintSubType::Hinge: {
                                auto* hinge = static_cast<JPH::HingeConstraint*>(constraint);
                                hinge->SetMotorState(JPH::EMotorState::Position);
                                hinge->SetTargetAngle(c.targetValue);
                                break;
                            }
                            case JPH::EConstraintSubType::Slider: {
                                auto* slider = static_cast<JPH::SliderConstraint*>(constraint);
                                slider->SetMotorState(JPH::EMotorState::Position);
                                slider->SetTargetPosition(c.targetValue);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                break;
            }
        }
    }
}

} // namespace ZHLN::Physics
