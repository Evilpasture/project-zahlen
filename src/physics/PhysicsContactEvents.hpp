// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PhysicsWorld.hpp"
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Physics/PhysicsHandles.hpp>
#include <atomic>
#include <cmath>

namespace ZHLN::Physics {

class ContactListener final: public JPH::ContactListener {
  public:
    explicit ContactListener(PhysicsWorld* world): _world(world) {
    }
    ~ContactListener() override;

    auto OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg, const JPH::CollideShapeResult&) -> JPH::ValidateResult override {
        const uint32_t d1 = GetDense(b1.GetID());
        const uint32_t d2 = GetDense(b2.GetID());
        if (d1 == 0xFFFFFFFFu || d2 == 0xFFFFFFFFu) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        const auto& bt = _world->bodyTable;
        if ((bt.categories[d1] & bt.masks[d2]) == 0u || (bt.categories[d2] & bt.masks[d1]) == 0u) {
            return JPH::ValidateResult::RejectContact;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        Record(ContactType::Added, b1, b2, m);
    }

    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings&) override {
        Record(ContactType::Persisted, b1, b2, m);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        const size_t idx = _world->contactCount.fetch_add(1, std::memory_order::relaxed);
        if (idx >= _world->contactCapacity) {
            return;
        }
        ContactEvent& ev = _world->contactBuffer[idx];
        ev.type          = ContactType::Removed;
        ev.body1         = GetHandle(pair.GetBody1ID());
        ev.body2         = GetHandle(pair.GetBody2ID());
    }

  private:
    PhysicsWorld* _world;

    static auto DecodeHandle(uint64_t raw) noexcept -> BodyHandle {
        return raw != 0 ? BodyHandle::Unpack(raw) : BodyHandle::Null();
    }

    auto GetDense(JPH::BodyID id) const noexcept -> uint32_t {
        const auto& bt     = _world->bodyTable;
        const auto  handle = GetHandle(id);
        if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::relaxed) != handle.generation) {
            return 0xFFFFFFFFu;
        }
        return bt.slotToDense[handle.index];
    }

    auto GetHandle(JPH::BodyID id) const noexcept -> BodyHandle {
        const uint32_t jIdx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        return DecodeHandle(_world->bodyTable.idToHandleMap[jIdx].load(std::memory_order::relaxed));
    }

    void Record(ContactType type, const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold) noexcept {
        const size_t idx = _world->contactCount.fetch_add(1, std::memory_order::relaxed);
        if (idx >= _world->contactCapacity) {
            return;
        }

        ContactEvent& ev = _world->contactBuffer[idx];
        ev.type          = type;

        const uint64_t r1 = b1.GetUserData();
        const uint64_t r2 = b2.GetUserData();
        JPH::Vec3      n  = manifold.mWorldSpaceNormal;

        if (r1 > r2) {
            ev.body1 = DecodeHandle(r2);
            ev.body2 = DecodeHandle(r1);
            n        = -n;
        } else {
            ev.body1 = DecodeHandle(r1);
            ev.body2 = DecodeHandle(r2);
        }

        const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        ev.px              = p.GetX();
        ev.py              = p.GetY();
        ev.pz              = p.GetZ();
        ev.nx              = n.GetX();
        ev.ny              = n.GetY();
        ev.nz              = n.GetZ();

        if (b1.IsSensor() || b2.IsSensor()) {
            ev.impulse      = 0.0f;
            ev.slidingSpeed = 0.0f;
        } else {
            const JPH::Vec3 v1        = b1.IsStatic() ? JPH::Vec3::sZero() : b1.GetPointVelocity(p);
            const JPH::Vec3 v2        = b2.IsStatic() ? JPH::Vec3::sZero() : b2.GetPointVelocity(p);
            const JPH::Vec3 dv        = (r1 > r2) ? (v2 - v1) : (v1 - v2);
            const float     normalVel = dv.Dot(n);
            ev.impulse                = std::abs(normalVel);
            ev.slidingSpeed           = dv.LengthSq() - (normalVel * normalVel);
        }
        std::atomic_thread_fence(std::memory_order::release);
    }
};

class CharacterListener final: public JPH::CharacterContactListener {
  public:
    explicit CharacterListener(PhysicsWorld* world): _world(world) {
    }
    ~CharacterListener() override;

    auto OnContactValidate(const JPH::CharacterVirtual* inChar, const JPH::CharacterContact& inContact) -> bool override {
        return Filter(inChar->GetUserData(), inContact.mBodyB);
    }

    auto OnCharacterContactValidate(const JPH::CharacterVirtual* inChar, const JPH::CharacterContact& inContact) -> bool override {
        return Filter(inChar->GetUserData(), inContact.mCharacterB->GetUserData());
    }

  private:
    PhysicsWorld* _world;

    auto Filter(uint64_t u1, JPH::BodyID id2) const noexcept -> bool {
        const auto&    bt    = _world->bodyTable;
        const uint32_t jIdx2 = id2.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        const uint64_t u2    = bt.idToHandleMap[jIdx2].load(std::memory_order::relaxed);
        if (u2 == 0) {
            return false;
        }

        const uint32_t d1 = bt.slotToDense[BodyHandle::Unpack(u1).index];
        const uint32_t d2 = bt.slotToDense[BodyHandle::Unpack(u2).index];
        return ((bt.categories[d1] & bt.masks[d2]) != 0u) && ((bt.categories[d2] & bt.masks[d1]) != 0u);
    }

    auto Filter(uint64_t u1, uint64_t u2) const noexcept -> bool {
        if (u1 == 0 || u2 == 0) {
            return false;
        }
        const auto&    bt = _world->bodyTable;
        const uint32_t d1 = bt.slotToDense[BodyHandle::Unpack(u1).index];
        const uint32_t d2 = bt.slotToDense[BodyHandle::Unpack(u2).index];
        return ((bt.categories[d1] & bt.masks[d2]) != 0u) && ((bt.categories[d2] & bt.masks[d1]) != 0u);
    }
};

} // namespace ZHLN::Physics
