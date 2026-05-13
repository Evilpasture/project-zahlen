#include "Physics.hpp"
#include "PhysicsWorld.hpp"

namespace ZHLN::Physics {

class ContactListener final : public JPH::ContactListener {
  public:
	explicit ContactListener(PhysicsWorld* world) : _world(world) {}

	virtual JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2,
												  JPH::RVec3Arg,
												  const JPH::CollideShapeResult&) override {
		uint32_t d1 = GetDense(b1.GetID());
		uint32_t d2 = GetDense(b2.GetID());
		if (d1 == 0xFFFFFFFF || d2 == 0xFFFFFFFF) {
			return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		if ((_world->categories[d1] & _world->masks[d2]) == 0 ||
			(_world->categories[d2] & _world->masks[d1]) == 0) {
			return JPH::ValidateResult::RejectContact;
		}
		return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
	}

	virtual void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
								const JPH::ContactManifold& m, JPH::ContactSettings&) override {
		Record(ContactType::Added, b1, b2, m);
	}

	virtual void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2,
									const JPH::ContactManifold& m, JPH::ContactSettings&) override {
		Record(ContactType::Persisted, b1, b2, m);
	}

	virtual void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
		size_t idx = _world->contactCount.fetch_add(1, std::memory_order_relaxed);
		if (idx >= _world->contactCapacity) {
			return;
		}
		ContactEvent& ev = _world->contactBuffer[idx];
		ev.type = ContactType::Removed;
		ev.body1 = GetHandle(pair.GetBody1ID());
		ev.body2 = GetHandle(pair.GetBody2ID());
	}

  private:
	PhysicsWorld* _world;

	uint32_t GetDense(JPH::BodyID id) {
		uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
		ZHLN::Entity h =
			ZHLN::Entity::Unpack(_world->idToHandleMap[j_idx].load(std::memory_order_relaxed));
		return (h.index < _world->slotCapacity) ? _world->slotToDense[h.index] : 0xFFFFFFFF;
	}

	ZHLN::Entity GetHandle(JPH::BodyID id) {
		uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
		return ZHLN::Entity::Unpack(_world->idToHandleMap[j_idx].load(std::memory_order_relaxed));
	}

	void Record(ContactType type, const JPH::Body& b1, const JPH::Body& b2,
				const JPH::ContactManifold& manifold) noexcept {
		size_t idx = _world->contactCount.fetch_add(1, std::memory_order_relaxed);
		if (idx >= _world->contactCapacity)
			return;

		ContactEvent& ev = _world->contactBuffer[idx];
		ev.type = type;

		uint64_t r1 = b1.GetUserData();
		uint64_t r2 = b2.GetUserData();
		JPH::Vec3 n = manifold.mWorldSpaceNormal;

		if (r1 > r2) {
			ev.body1 = ZHLN::Entity::Unpack(r2);
			ev.body2 = ZHLN::Entity::Unpack(r1);
			n = -n;
		} else {
			ev.body1 = ZHLN::Entity::Unpack(r1);
			ev.body2 = ZHLN::Entity::Unpack(r2);
		}

		const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
		ev.px = p.GetX();
		ev.py = p.GetY();
		ev.pz = p.GetZ();
		ev.nx = n.GetX();
		ev.ny = n.GetY();
		ev.nz = n.GetZ();

		if (b1.IsSensor() || b2.IsSensor()) {
			ev.impulse = 0.0f;
			ev.slidingSpeed = 0.0f;
		} else {
			JPH::Vec3 v1 = b1.IsStatic() ? JPH::Vec3::sZero() : b1.GetPointVelocity(p);
			JPH::Vec3 v2 = b2.IsStatic() ? JPH::Vec3::sZero() : b2.GetPointVelocity(p);
			JPH::Vec3 dv = (r1 > r2) ? (v2 - v1) : (v1 - v2);
			float normalVel = dv.Dot(n);
			ev.impulse = std::abs(normalVel);
			ev.slidingSpeed = dv.LengthSq() - (normalVel * normalVel);
		}
		std::atomic_thread_fence(std::memory_order_release);
	}
};

class CharacterListener final : public JPH::CharacterContactListener {
  public:
	explicit CharacterListener(PhysicsWorld* world) : _world(world) {}

	virtual bool OnContactValidate(const JPH::CharacterVirtual* inChar,
								   const JPH::CharacterContact& inContact) override {
		return Filter(inChar->GetUserData(), inContact.mBodyB);
	}

	virtual bool OnCharacterContactValidate(const JPH::CharacterVirtual* inChar,
											const JPH::CharacterContact& inContact) override {
		return Filter(inChar->GetUserData(), inContact.mCharacterB->GetUserData());
	}

	virtual void OnContactAdded(const JPH::CharacterVirtual* inChar,
								const JPH::CharacterContact& inContact,
								JPH::CharacterContactSettings& ioSettings) override {
		ApplyPushImpulse(inChar, inContact);
	}

	virtual void OnContactPersisted(const JPH::CharacterVirtual* inChar,
									const JPH::CharacterContact& inContact,
									JPH::CharacterContactSettings& ioSettings) override {
		ApplyPushImpulse(inChar, inContact);
	}

	virtual void OnAdjustBodyVelocity(const JPH::CharacterVirtual* inChar, const JPH::Body& inBody2,
									  JPH::Vec3& ioLinVel, JPH::Vec3& ioAngVel) override {
		// Inherit tangential velocity from rotating platforms (Culverin-style)
		JPH::Vec3 omega = inBody2.GetAngularVelocity();
		JPH::RVec3 delta = inChar->GetPosition() - inBody2.GetPosition();

		// v = omega x r
		ioLinVel.SetX(omega.GetY() * (float)delta.GetZ());
		ioLinVel.SetZ(-omega.GetY() * (float)delta.GetX());
		ioAngVel.SetY(omega.GetY());
	}

  private:
	PhysicsWorld* _world;

	bool Filter(uint64_t u1, JPH::BodyID id2) {
		uint32_t j_idx2 = id2.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
		uint64_t u2 = _world->idToHandleMap[j_idx2].load(std::memory_order_relaxed);
		if (u2 == 0)
			return true;

		uint32_t d1 = _world->slotToDense[ZHLN::Entity::Unpack(u1).index];
		uint32_t d2 = _world->slotToDense[ZHLN::Entity::Unpack(u2).index];
		return (_world->categories[d1] & _world->masks[d2]) &&
			   (_world->categories[d2] & _world->masks[d1]);
	}

	bool Filter(uint64_t u1, uint64_t u2) {
		uint32_t d1 = _world->slotToDense[ZHLN::Entity::Unpack(u1).index];
		uint32_t d2 = _world->slotToDense[ZHLN::Entity::Unpack(u2).index];
		return (_world->categories[d1] & _world->masks[d2]) &&
			   (_world->categories[d2] & _world->masks[d1]);
	}

	void ApplyPushImpulse(const JPH::CharacterVirtual* inChar,
						  const JPH::CharacterContact& inContact) {
		if (inContact.mIsSensorB || inContact.mMotionTypeB != JPH::EMotionType::Dynamic)
			return;

		JPH::Vec3 charVel = inChar->GetLinearVelocity();
		float dot = charVel.Dot(inContact.mContactNormal);

		// Normal points towards character, so dot < 0 means moving into object
		if (dot < -0.01f) {
			JPH::Vec3 impulse =
				inContact.mContactNormal * (dot * 100.0f); // 100.0f is push strength
			impulse.SetY(std::max(0.0f, impulse.GetY()));  // Don't push objects into the floor
			_world->bodyInterface->AddImpulse(inContact.mBodyB, -impulse);
		}
	}
};

} // namespace ZHLN::Physics
