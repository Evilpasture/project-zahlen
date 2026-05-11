#include "Physics.hpp"
#include "PhysicsWorld.hpp"

namespace ZHLN::Physics {

class ContactListener final : public JPH::ContactListener {
  public:
	explicit ContactListener(PhysicsWorld* world) : _world(world) {}

	virtual void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
								const JPH::ContactManifold& manifold,
								JPH::ContactSettings& ioSettings) override {
		RecordContact(ContactType::Added, body1, body2, manifold);
	}

	virtual void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
									const JPH::ContactManifold& manifold,
									JPH::ContactSettings& ioSettings) override {
		RecordContact(ContactType::Persisted, body1, body2, manifold);
	}

	virtual void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override {
		// Removal provides less data (no manifold).
		// We could decode the Jolt IDs to Handles via the idToHandleMap here if needed,
		// but typically, knowing *that* a collision ended is enough.
		// For brevity, we skip populating the removal event payload here,
		// but the architecture fully supports it via _world->idToHandleMap.
	}

  private:
	PhysicsWorld* _world;

	// The unified, lock-free event writer
	[[gnu::always_inline]]
	void RecordContact(ContactType type, const JPH::Body& b1, const JPH::Body& b2,
					   const JPH::ContactManifold& manifold) noexcept {

		size_t idx = _world->contactCount.fetch_add(1, std::memory_order_relaxed);
		if (idx >= _world->contactCapacity) [[unlikely]]
			return;

		ContactEvent& ev = _world->contactBuffer[idx];
		ev.type = type;

		// 1. Identity & Normal Mapping
		uint64_t r1 = b1.GetUserData();
		uint64_t r2 = b2.GetUserData();
		JPH::Vec3 n = manifold.mWorldSpaceNormal;

		if (r1 > r2) {
			ev.body1 = EntityHandle::Unpack(r2);
			ev.body2 = EntityHandle::Unpack(r1);
			n = -n;
		} else {
			ev.body1 = EntityHandle::Unpack(r1);
			ev.body2 = EntityHandle::Unpack(r2);
		}

		ev.nx = n.GetX();
		ev.ny = n.GetY();
		ev.nz = n.GetZ();

		// 2. Spatial Point
		const JPH::RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
		ev.px = p.GetX();
		ev.py = p.GetY();
		ev.pz = p.GetZ();

		// 3. Advanced Dynamics (Relative Velocity)
		// We calculate velocity at the specific contact point 'p'
		JPH::Vec3 v1 = b1.IsStatic() ? JPH::Vec3::sZero() : b1.GetPointVelocity(p);
		JPH::Vec3 v2 = b2.IsStatic() ? JPH::Vec3::sZero() : b2.GetPointVelocity(p);

		// dv = velocity of body1 relative to body2
		JPH::Vec3 dv = (r1 > r2) ? (v2 - v1) : (v1 - v2);

		ev.rvx = dv.GetX();
		ev.rvy = dv.GetY();
		ev.rvz = dv.GetZ();

		float normalVel = dv.Dot(n);
		ev.impulse = std::abs(normalVel);

		// Sliding speed = length of the velocity vector projected onto the contact plane
		ev.slidingSpeed = (dv - n * normalVel).LengthSq();

		// 4. Metadata (Placeholder for your material system)
		ev.mat1 = 0; // Populate from b1.GetShape()->GetMaterial(sub1)...
		ev.mat2 = 0;
	}
};

} // namespace ZHLN::Physics