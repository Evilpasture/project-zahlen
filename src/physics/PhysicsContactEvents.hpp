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
		size_t idx = _world->contactCount.fetch_add(1, std::memory_order_relaxed);
		if (idx >= _world->contactCapacity)
			return;

		ContactEvent& ev = _world->contactBuffer[idx];
		ev.type = ContactType::Removed;

		// Decode Handles from Jolt BodyIDs using the fast-map
		auto getHandle = [&](JPH::BodyID id) {
			uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
			return EntityHandle::Unpack(
				_world->idToHandleMap[j_idx].load(std::memory_order_relaxed));
		};

		EntityHandle h1 = getHandle(inSubShapePair.GetBody1ID());
		EntityHandle h2 = getHandle(inSubShapePair.GetBody2ID());

		// Sort to maintain consistency
		if (h1.Pack() > h2.Pack()) {
			ev.body1 = h2;
			ev.body2 = h1;
		} else {
			ev.body1 = h1;
			ev.body2 = h2;
		}

		ev.sub1 = inSubShapePair.GetSubShapeID1().GetValue();
		ev.sub2 = inSubShapePair.GetSubShapeID2().GetValue();
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

		// 4. Metadata Extraction
		ev.sub1 = manifold.mSubShapeID1.GetValue();
		ev.sub2 = manifold.mSubShapeID2.GetValue();

		// Map Jolt Material to an integer ID (requires a custom Material class or hashing)
		auto getMatId = [](const JPH::Body& b, const JPH::SubShapeID& sub) {
			const JPH::Shape* shape = b.GetShape();
			const JPH::PhysicsMaterial* mat = shape->GetMaterial(sub);
			return (mat != nullptr)
					   ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mat) & 0xFFFFFFFF)
					   : 0;
		};

		ev.mat1 = getMatId(b1, manifold.mSubShapeID1);
		ev.mat2 = getMatId(b2, manifold.mSubShapeID2);
	}
};

} // namespace ZHLN::Physics
