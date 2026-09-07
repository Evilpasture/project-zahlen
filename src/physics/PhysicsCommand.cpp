// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include "Zahlen/Log.hpp"
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Zahlen/physics/Physics.hpp> // For GetBodyID
#include <algorithm>

namespace ZHLN::Physics {

void PhysicsWorld::FlushCommands(
    Command*                                     capturedQueue,
    size_t                                       capturedCount,
    JPH::Array<JPH::Ref<JPH::CharacterVirtual>>& characterMap,
    JPH::Array<JPH::CharacterVirtual*>&          activeCharacters
) {
    if (capturedCount == 0) {
        return;
    }

    for (size_t i = 0; i < capturedCount; i++) {
        const auto& cmd = capturedQueue[i];

        switch (cmd.type) {
            case CommandType::DestroyBody: {
                const uint32_t slot = cmd.handle.index;
                if (generations[slot].load(std::memory_order::acquire) != cmd.handle.generation) {
                    continue;
                }

                const uint32_t    dense  = slotToDense[slot];
                const JPH::BodyID bodyID = bodyIDs[dense];

                if (bodyID.IsInvalid()) {
                    // Virtual characters are physics slots too, but have no
                    // JPH::BodyID. Release their strong reference and remove
                    // the raw update-list entry before recycling the slot.
                    if (slot < characterMap.size() && characterMap[slot] != nullptr) {
                        auto* character = characterMap[slot].GetPtr();
                        character->SetListener(nullptr);
                        activeCharacters.erase(std::remove(activeCharacters.begin(), activeCharacters.end(), character), activeCharacters.end());
                        characterMap[slot] = nullptr;
                    }
                    RemoveBodySlot(slot);
                    break;
                }

                const uint32_t joltIdx = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;

                // Verify indices against array boundaries before writing
                ZHLN::Assert(
                    joltIdx < idToHandleMap.size() && joltIdx < joltBodyPtrs.size(),
                    "PhysicsCommand: joltIdx ({}) exceeds active map sizes ({}, {}) during DestroyBody!", joltIdx, idToHandleMap.size(), joltBodyPtrs.size()
                );

                idToHandleMap[joltIdx].store(0, std::memory_order::release);
                joltBodyPtrs[joltIdx] = nullptr;

                bodyInterface->RemoveBody(bodyID);
                bodyInterface->DestroyBody(bodyID);
                RemoveBodySlot(slot);
                break;
            }

            case CommandType::CreateConstraint: {
                // FIXED: Pass `*this` as the world to the free function `GetBodyID`
                JPH::BodyID id1 = GetBodyID(*this, cmd.createC.b1);
                JPH::BodyID id2 = GetBodyID(*this, cmd.createC.b2);

                JPH::BodyLockWrite lock1(system->GetBodyLockInterface(), id1);
                JPH::BodyLockWrite lock2(system->GetBodyLockInterface(), id2);

                if (lock1.Succeeded() && lock2.Succeeded()) {
                    auto* joltConstraint = CreateNativeConstraint(cmd.createC.cType, &lock1.GetBody(), &lock2.GetBody(), cmd.createC.params);

                    if (joltConstraint != nullptr) {
                        joltConstraint->AddRef();
                        system->AddConstraint(joltConstraint);

                        uint32_t slot          = cmd.cHandle.index;
                        constraints[slot]      = joltConstraint;
                        constraintStates[slot] = SLOT_ALIVE;
                    }
                }
                break;
            }

            case CommandType::DestroyConstraint: {
                const uint32_t slot = cmd.cHandle.index;
                if (constraintGenerations[slot].load(std::memory_order::acquire) == cmd.cHandle.generation) {
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
                uint32_t slot = cmd.setTarget.targetCHandle.index;
                if (constraintGenerations[slot].load(std::memory_order::relaxed) == cmd.setTarget.targetCHandle.generation) {
                    JPH::Constraint* c = constraints[slot];
                    if (c != nullptr) {
                        switch (c->GetSubType()) {
                            case JPH::EConstraintSubType::Hinge: {
                                auto* hinge = static_cast<JPH::HingeConstraint*>(c);
                                hinge->SetMotorState(JPH::EMotorState::Position);
                                hinge->SetTargetAngle(cmd.setTarget.targetValue);
                                break;
                            }
                            case JPH::EConstraintSubType::Slider: {
                                auto* slider = static_cast<JPH::SliderConstraint*>(c);
                                slider->SetMotorState(JPH::EMotorState::Position);
                                slider->SetTargetPosition(cmd.setTarget.targetValue);
                                break;
                            }
                            default:
                                break;
                        }
                    }
                }
                break;
            }

            case CommandType::SetCollisionFilter: {
                const uint32_t slot = cmd.setFilter.handle.index;
                if (generations[slot].load(std::memory_order::acquire) == cmd.setFilter.handle.generation) {
                    uint32_t dense    = slotToDense[slot];
                    categories[dense] = cmd.setFilter.category;
                    masks[dense]      = cmd.setFilter.mask;
                }
                break;
            }
        }
    }
}

} // namespace ZHLN::Physics
