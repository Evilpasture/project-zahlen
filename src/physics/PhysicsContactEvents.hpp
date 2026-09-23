// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN::Physics {

class ContactListener final: public JPH::ContactListener {
  public:
    explicit ContactListener(PhysicsWorld* world): _world(world) {
    }

    auto OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg /*inBaseOffset*/, const JPH::CollideShapeResult& /*inCollisionResult*/)
        -> JPH::ValidateResult override {
        uint32_t d1 = GetDense(b1.GetID());
        uint32_t d2 = GetDense(b2.GetID());
        if (d1 == 0xFFFFFFFF || d2 == 0xFFFFFFFF) {
            return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
        }

        if ((_world->categories[d1] & _world->masks[d2]) == 0 || (_world->categories[d2] & _world->masks[d1]) == 0) {
            return JPH::ValidateResult::RejectContact;
        }
        return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings& /*ioSettings*/) override {
        Record(ContactType::Added, b1, b2, m);
    }

    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m, JPH::ContactSettings& /*ioSettings*/) override {
        Record(ContactType::Persisted, b1, b2, m);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        size_t idx = _world->contactCount.fetch_add(1, std::memory_order::relaxed);
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

    auto GetDense(JPH::BodyID id) -> uint32_t {
        uint32_t     j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        ZHLN::Entity h     = ZHLN::Entity::Unpack(_world->idToHandleMap[j_idx].load(std::memory_order::relaxed));
        return (h.index < _world->slotCapacity) ? _world->slotToDense[h.index] : 0xFFFFFFFF;
    }

    auto GetHandle(JPH::BodyID id) -> ZHLN::Entity {
        uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        return ZHLN::Entity::Unpack(_world->idToHandleMap[j_idx].load(std::memory_order::relaxed));
    }

    void Record(ContactType type, const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& manifold) noexcept {
        size_t idx = _world->contactCount.fetch_add(1, std::memory_order::relaxed);
        if (idx >= _world->contactCapacity) {
            return;
        }

        ContactEvent& ev = _world->contactBuffer[idx];
        ev.type          = type;

        uint64_t  r1 = b1.GetUserData();
        uint64_t  r2 = b2.GetUserData();
        JPH::Vec3 n  = manifold.mWorldSpaceNormal;

        if (r1 > r2) {
            ev.body1 = ZHLN::Entity::Unpack(r2);
            ev.body2 = ZHLN::Entity::Unpack(r1);
            n        = -n;
        } else {
            ev.body1 = ZHLN::Entity::Unpack(r1);
            ev.body2 = ZHLN::Entity::Unpack(r2);
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
            JPH::Vec3 v1        = b1.IsStatic() ? JPH::Vec3::sZero() : b1.GetPointVelocity(p);
            JPH::Vec3 v2        = b2.IsStatic() ? JPH::Vec3::sZero() : b2.GetPointVelocity(p);
            JPH::Vec3 dv        = (r1 > r2) ? (v2 - v1) : (v1 - v2);
            float     normalVel = dv.Dot(n);
            ev.impulse          = std::abs(normalVel);
            ev.slidingSpeed     = dv.LengthSq() - (normalVel * normalVel);
        }
        std::atomic_thread_fence(std::memory_order::release);
    }
};

class CharacterListener final: public JPH::CharacterContactListener {
  public:
    explicit CharacterListener(PhysicsWorld* world): _world(world) {
    }

    auto OnContactValidate(const JPH::CharacterVirtual* inChar, const JPH::CharacterContact& inContact) -> bool override {
        return Filter(inChar->GetUserData(), inContact.mBodyB);
    }

    auto OnCharacterContactValidate(const JPH::CharacterVirtual* inChar, const JPH::CharacterContact& inContact) -> bool override {
        return Filter(inChar->GetUserData(), inContact.mCharacterB->GetUserData());
    }

    // Note: OnAdjustBodyVelocity is intentionally left to Jolt's default. The
    // contact velocity is already computed from the body's real linear and
    // angular velocity (io velocity is filled in before the callback), so a
    // character standing on a moving or rotating platform is carried with it
    // with no override. Overriding it to re-add the rotational surface
    // velocity double-counts the spin. Likewise the contact-added/persisted
    // callbacks are left to their (empty) defaults: gameplay impulses a
    // character exerts on props are controller policy, applied in the
    // character layer's fixed-step hook through the public physics API, not
    // hidden inside a contact callback.

  private:
    PhysicsWorld* _world;

    auto Filter(uint64_t u1, JPH::BodyID id2) -> bool {
        uint32_t j_idx2 = id2.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
        uint64_t u2     = _world->idToHandleMap[j_idx2].load(std::memory_order::relaxed);
        if (u2 == 0) {
            return false;
        }

        uint32_t d1 = _world->slotToDense[ZHLN::Entity::Unpack(u1).index];
        uint32_t d2 = _world->slotToDense[ZHLN::Entity::Unpack(u2).index];
        return ((_world->categories[d1] & _world->masks[d2]) != 0u) && ((_world->categories[d2] & _world->masks[d1]) != 0u);
    }

    auto Filter(uint64_t u1, uint64_t u2) -> bool {
        if (u1 == 0 || u2 == 0) {
            return false;
        }
        uint32_t d1 = _world->slotToDense[ZHLN::Entity::Unpack(u1).index];
        uint32_t d2 = _world->slotToDense[ZHLN::Entity::Unpack(u2).index];
        return ((_world->categories[d1] & _world->masks[d2]) != 0u) && ((_world->categories[d2] & _world->masks[d1]) != 0u);
    }
};

} // namespace ZHLN::Physics
