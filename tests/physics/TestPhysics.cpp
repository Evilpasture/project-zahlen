// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/CommonFixtures.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <atomic>
#include <cmath>
#include <expected>
#include <thread>
#include <vector>

enum class PhysicsTestError : uint32_t {
    RaycastMissedExpectedBody ZHLN_ANNOTATION(ZHLN::Description<"Raycast did not hit expected collider.">{}) = 1,
    OverlapQueryFailed ZHLN_ANNOTATION(ZHLN::Description<"Broadphase overlap failed to detect sphere/AABB collision.">{}),
    RagdollCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Creating a single-part ragdoll fixture failed.">{}),
    ParallelCharacterSyncFailed ZHLN_ANNOTATION(ZHLN::Description<"Parallel character step or concurrent shadow-state query failed.">{}),
    LockFreeConcurrencyFailed ZHLN_ANNOTATION(ZHLN::Description<"Concurrent ping-pong transform reads or mailbox commands failed during Step.">{}),
};

namespace ZHLN::Test {
namespace ZHLN_ANNOTATION(UseFixtures<Fixture::TaskSystemAndJoltScope> {}) {

        // The explicit fixture parameter guarantees workers and Jolt types
        // even when an unnamed namespace's ambient mark is not reflected.
        std::expected<void, ZHLN::ErrorCode> test_rigid_body_raycast_and_overlap(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 128, .maxBodyPairs = 256, .maxContactConstraints = 256, .tempAllocatorSize = 4 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // Create static ground box at (0, 0, 0) with half-extents (10, 1, 10)
            auto         boxShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 10.0f, 1.0f, 10.0f);
            ZHLN::Physics::BodyHandle ground = pc.CreateRigidBody(boxShape, JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            ZHLN::Test::Expect(ground) != ZHLN::Physics::BodyHandle::Null();

            pc.OptimizeBroadphase();

            // 1. Raycast downwards from (0, 10, 0) -> (0, -1, 0)
            auto hit = pc.Raycast(JPH::RVec3(0, 10, 0), JPH::Vec3(0, -1, 0), 20.0f);
            ZHLN::Test::Expect(hit.hasHit);
            ZHLN::Test::Expect(hit.handle.Pack()) == ground.Pack();

            // Ground box top surface is at Y = 1.0
            ZHLN::Test::Expect(hit.position.GetY()) == ZHLN::Test::Approx(1.0f, 0.05f);
            ZHLN::Test::Expect(hit.normal.GetY()) > 0.9f;

            // 2. Sphere overlap at (0, 0, 0)
            JPH::Array<ZHLN::Physics::BodyHandle> overlapResults;
            pc.OverlapSphere(JPH::RVec3(0, 0, 0), 5.0f, overlapResults);
            ZHLN::Test::ExpectGe(overlapResults.size(), 1);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_dynamic_body_simulation_step(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 128, .maxBodyPairs = 256, .maxContactConstraints = 256, .tempAllocatorSize = 4 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // Create a dynamic falling sphere (radius 0.5) centered at (0, 10, 0)
            // Top surface initially at Y = 10.5
            auto         sphereShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.5f);
            ZHLN::Physics::BodyHandle sphere =
                pc.CreateRigidBody(sphereShape, JPH::RVec3(0, 10, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            ZHLN::Test::ExpectTrue(sphere != ZHLN::Physics::BodyHandle::Null());

            pc.OptimizeBroadphase();

            // Step 30 physics frames (0.5s total): falls d = 0.5 * 9.81 * 0.25 = ~1.23m
            // New center Y ~= 8.77m, new top surface Y ~= 9.27m
            for (int i = 0; i < 30; ++i) {
                pc.Step(1.0f / 60.0f);
            }

            // Raycast down from (0, 15, 0) to find new fallen position
            auto hit = pc.Raycast(JPH::RVec3(0, 15, 0), JPH::Vec3(0, -1, 0), 30.0f);
            ZHLN::Test::ExpectTrue(hit.hasHit);
            ZHLN::Test::ExpectLt(hit.position.GetY(), 10.0f); // Top surface fell below 10.0m

            // The public physics façade exposes the synchronized position without
            // leaking PhysicsWorld's slot-to-dense mapping or SoA storage.
            const auto synchronizedPosition = pc.BodyPosition(sphere);
            ZHLN::Test::ExpectTrue(synchronizedPosition.has_value());
            ZHLN::Test::ExpectLt(synchronizedPosition->GetY(), 9.0);

            const auto bodyState = pc.BodyState(sphere);
            ZHLN::Test::ExpectTrue(bodyState.has_value());
            ZHLN::Test::ExpectLt(bodyState->currentPosition.GetY(), 9.0f);
            ZHLN::Test::ExpectGt(bodyState->previousPosition.GetY(), bodyState->currentPosition.GetY());

            // A queued body destruction invalidates the generation-safe lookup
            // once the following physics step has drained its command queue.
            pc.DestroyBody(sphere);
            pc.Step(1.0f / 60.0f);
            ZHLN::Test::ExpectTrue(!pc.BodyPosition(sphere).has_value());
            ZHLN::Test::ExpectTrue(!pc.BodyState(sphere).has_value());

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_explicit_body_detach_and_batched_destruction(Fixture::TaskSystemAndJoltScope&) {
            using Body = ZHLN::Components::PhysicsComponent;
            using Handle = ZHLN::Physics::BodyHandle;
            ZHLN::PhysicsConfig cfg {.maxBodies = 16, .maxBodyPairs = 32, .maxContactConstraints = 32, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);
            ZHLN::ECS::Registry registry; // destroyed before pc
            const auto shape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);
            auto createBody = [&] {
                return pc.CreateRigidBody(shape, JPH::RVec3(0, 1, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            };

            const auto firstBody = createBody();
            const auto owner = registry.Create(Body {.physicsHandle = firstBody});
            ZHLN::Test::ExpectTrue(ZHLN::SceneResources::Detach<Body>(pc, registry, owner));
            ZHLN::Test::ExpectFalse(ZHLN::SceneResources::Detach<Body>(pc, registry, owner));
            pc.Step(1.0f / 60.0f);
            ZHLN::Test::ExpectEq(pc.GetActiveBodyCount(), 0u);
            ZHLN::Test::ExpectFalse(pc.IsBodyDynamic(firstBody));

            const auto secondBody = createBody();
            ZHLN::SceneResources::Attach(pc, registry, owner, Body {.physicsHandle = secondBody});
            const auto thirdBody = createBody();
            ZHLN::SceneResources::Attach(pc, registry, owner, Body {.physicsHandle = thirdBody}); // releases second
            pc.Step(1.0f / 60.0f);
            ZHLN::Test::ExpectFalse(pc.IsBodyDynamic(secondBody));
            ZHLN::Test::ExpectTrue(pc.IsBodyDynamic(thirdBody));

            // A scene query observes intact components; a single batched call
            // queues the releases before the registry reclaims their entities.
            const auto otherBody = createBody();
            const auto other = registry.Create(Body {.physicsHandle = otherBody});
            registry.Add(owner, ZHLN::Components::PendingDestroy {});
            registry.Add(other, ZHLN::Components::PendingDestroy {});
            ZHLN::Test::ExpectEq(registry.Get<Body>(owner)->physicsHandle, thirdBody);
            const std::vector<Handle> batch {thirdBody, otherBody, thirdBody, Handle::Null(), secondBody};
            pc.DestroyBodies(batch); // duplicate, null and stale handles are safe
            const auto marked = registry.GetEntitiesWith<ZHLN::Components::PendingDestroy>();
            const std::vector<ZHLN::Entity> pending(marked.begin(), marked.end());
            for (auto entity: pending) {
                registry.Destroy(entity);
            }
            pc.Step(1.0f / 60.0f);
            ZHLN::Test::ExpectEq(pc.GetActiveBodyCount(), 0u);

            const auto lastBody = createBody();
            registry.Create(Body {.physicsHandle = lastBody});
            pc.DestroyBodies(std::span {&lastBody, 1});
            registry.Clear(); // data-only storage reset after explicit release
            pc.Step(1.0f / 60.0f);
            ZHLN::Test::ExpectEq(pc.GetActiveBodyCount(), 0u);
            ZHLN::Test::ExpectFalse(pc.IsBodyDynamic(lastBody));
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_ragdoll_handles_are_released_explicitly(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig cfg {.maxBodies = 16, .maxBodyPairs = 32, .maxContactConstraints = 32, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);
            ZHLN::ECS::Registry registry;

            JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton();
            skeleton->AddJoint("root", "");
            skeleton->CalculateParentJointIndices();
            const auto shape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.25f);
            std::vector<ZHLN::Physics::RagdollPartParams> parts {{.jointIndex = 0, .shape = shape}};

            const auto first = pc.CreateSkeletalRagdoll(skeleton, parts);
            if (!ZHLN::Test::ExpectTrue(first != ZHLN::Physics::RagdollHandle::Invalid)) {
                return std::unexpected(PhysicsTestError::RagdollCreationFailed);
            }
            const auto owner = registry.Create(ZHLN::Components::RagdollComponent {.ragdollHandle = first});
            ZHLN::Test::ExpectTrue(pc.GetRagdoll(first).has_value());
            ZHLN::SceneResources::Detach<ZHLN::Components::RagdollComponent>(pc, registry, owner);
            ZHLN::Test::ExpectFalse(pc.GetRagdoll(first).has_value());

            const auto second = pc.CreateSkeletalRagdoll(skeleton, parts);
            ZHLN::Test::ExpectTrue(second != first);
            ZHLN::SceneResources::Attach(pc, registry, owner, ZHLN::Components::RagdollComponent {.ragdollHandle = second});
            const std::vector<ZHLN::Physics::RagdollHandle> handles {second, first, second};
            pc.DestroyRagdolls(handles);
            registry.Clear();
            ZHLN::Test::ExpectFalse(pc.GetRagdoll(second).has_value());
            pc.DestroyRagdoll(second); // stale generations and repeated release are safe
            return {};
        }

        // Regression guard for PhysicsContext::IsBodyDynamic. The motion type
        // must be read from Jolt's authoritative body state (resolved through
        // the dense slot's BodyID), not from a raw pointer read off the
        // Jolt-indexed joltBodyPtrs array -- which the dense index does not
        // address and which stays null until a Step's sync pass runs. The
        // handle's generation must also be validated so a stale handle cannot
        // resolve to a slot's new occupant.
        std::expected<void, ZHLN::ErrorCode> test_is_body_dynamic_reports_motion_type_and_rejects_stale_handles(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig cfg {.maxBodies = 16, .maxBodyPairs = 32, .maxContactConstraints = 32, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            const auto sphereShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.5f);
            const auto boxShape    = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.5f, 0.5f, 0.5f);

            // The motion type is queryable as soon as the body exists in Jolt,
            // before any Step has populated the engine's shadow SoA. A dynamic
            // body is dynamic the moment it is created.
            const ZHLN::Physics::BodyHandle dynamicBody = pc.CreateRigidBody(sphereShape, JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            ZHLN::Test::ExpectTrue(dynamicBody != ZHLN::Physics::BodyHandle::Null());
            ZHLN::Test::ExpectTrue(pc.IsBodyDynamic(dynamicBody));

            const ZHLN::Physics::BodyHandle staticBody    = pc.CreateRigidBody(boxShape, JPH::RVec3(0, 5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            const ZHLN::Physics::BodyHandle kinematicBody = pc.CreateRigidBody(boxShape, JPH::RVec3(0, 10, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Kinematic, ZHLN::Layers::ID::MOVING);
            ZHLN::Test::ExpectTrue(staticBody != ZHLN::Physics::BodyHandle::Null());
            ZHLN::Test::ExpectTrue(kinematicBody != ZHLN::Physics::BodyHandle::Null());
            ZHLN::Test::ExpectTrue(!pc.IsBodyDynamic(staticBody));
            ZHLN::Test::ExpectTrue(!pc.IsBodyDynamic(kinematicBody));
            ZHLN::Test::ExpectTrue(!pc.IsBodyDynamic(ZHLN::Physics::BodyHandle::Null()));

            // A destroyed body's slot is the first to be handed back out (LIFO
            // free list) with a bumped generation. The stale handle keeps the
            // old generation, so it must not resolve to the new occupant even
            // though the slot index now matches a live, dynamic body.
            pc.DestroyBody(dynamicBody);
            pc.Step(1.0f / 60.0f);

            const ZHLN::Physics::BodyHandle reused = pc.CreateRigidBody(sphereShape, JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);
            ZHLN::Test::ExpectTrue(reused != ZHLN::Physics::BodyHandle::Null());
            ZHLN::Test::ExpectTrue(pc.IsBodyDynamic(reused));        // the live occupant is dynamic
            ZHLN::Test::ExpectTrue(!pc.IsBodyDynamic(dynamicBody));  // the stale handle is not
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_parallel_characters_and_concurrent_state_queries(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 64, .maxBodyPairs = 128, .maxContactConstraints = 128, .tempAllocatorSize = 2 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            const auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 40.0f, 0.5f, 40.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);
            pc.OptimizeBroadphase();

            constexpr size_t                        kCharCount = 8;
            std::vector<ZHLN::Physics::BodyHandle>  characters(kCharCount);
            for (size_t i = 0; i < kCharCount; ++i) {
                characters[i] = pc.CreateCharacter(JPH::RVec3(static_cast<double>(i) * 2.5, 1.5, 0.0));
                if (!ZHLN::Test::ExpectTrue(characters[i] != ZHLN::Physics::BodyHandle::Null())) {
                    return std::unexpected(PhysicsTestError::ParallelCharacterSyncFailed);
                }
            }

            for (int step = 0; step < 6; ++step) {
                pc.Step(1.0f / 60.0f);
            }

            std::vector<ZHLN::Physics::BodyStateSnapshot> snapshots(kCharCount);
            std::atomic<bool> shapesValid {true};
            ZHLN::TaskSystem::ParallelFor(static_cast<uint32_t>(kCharCount), 1, [&](uint32_t start, uint32_t end, uint32_t) {
                pc.FillBodyStates(
                    std::span<const ZHLN::Physics::BodyHandle>(characters.data() + start, end - start),
                    std::span<ZHLN::Physics::BodyStateSnapshot>(snapshots.data() + start, end - start)
                );
                const auto shape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.25f + static_cast<float>(start) * 0.1f);
                if (shape == nullptr) {
                    shapesValid.store(false, std::memory_order::relaxed);
                }
            });

            if (!ZHLN::Test::ExpectTrue(shapesValid.load(std::memory_order::relaxed))) {
                return std::unexpected(PhysicsTestError::ParallelCharacterSyncFailed);
            }

            for (size_t i = 0; i < kCharCount; ++i) {
                if (!ZHLN::Test::ExpectTrue(snapshots[i].valid && snapshots[i].isCharacter)) {
                    return std::unexpected(PhysicsTestError::ParallelCharacterSyncFailed);
                }
                const auto single = pc.BodyState(characters[i]);
                if (!ZHLN::Test::ExpectTrue(single.has_value() && single->valid)) {
                    return std::unexpected(PhysicsTestError::ParallelCharacterSyncFailed);
                }
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> test_lock_free_ping_pong_reads_and_concurrent_command_mailbox(Fixture::TaskSystemAndJoltScope&) {
            ZHLN::PhysicsConfig  cfg {.maxBodies = 128, .maxBodyPairs = 256, .maxContactConstraints = 256, .tempAllocatorSize = 4 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            const auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 50.0f, 0.5f, 50.0f);
            const auto sphereShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.35f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -0.5, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);

            constexpr size_t                       kPersistentBodies = 12;
            std::vector<ZHLN::Physics::BodyHandle> persistent(kPersistentBodies);
            for (size_t i = 0; i < kPersistentBodies; ++i) {
                persistent[i] = pc.CreateRigidBody(
                    sphereShape,
                    JPH::RVec3(static_cast<double>(i) * 1.25 - 6.0, 4.0 + static_cast<double>(i) * 0.25, 0.0),
                    JPH::Quat::sIdentity(),
                    JPH::EMotionType::Dynamic,
                    ZHLN::Layers::ID::MOVING
                );
            }

            const ZHLN::Physics::BodyHandle character = pc.CreateCharacter(JPH::RVec3(0.0, 0.8, 3.0));
            const float                     cachedGroundOffset = pc.GetCharacterGroundOffset(character);
            if (!ZHLN::Test::ExpectGt(cachedGroundOffset, 0.5f)) {
                return std::unexpected(PhysicsTestError::LockFreeConcurrencyFailed);
            }

            pc.OptimizeBroadphase();
            pc.Step(1.0f / 60.0f);

            std::atomic<bool>     keepRunning {true};
            std::atomic<bool>     workerFailed {false};
            std::atomic<bool>     observedGrounded {false};
            std::atomic<uint32_t> stepEpoch {1u};

            std::thread readerAndProducer([&] {
                std::vector<ZHLN::Physics::BodyStateSnapshot> snaps(kPersistentBodies);
                uint32_t                                      lastSpawnedEpoch = 0;
                while (keepRunning.load(std::memory_order::acquire)) {
                    pc.FillBodyStates(persistent, snaps);
                    for (const auto& s: snaps) {
                        if (!s.valid || !std::isfinite(s.currentPosition.GetX()) || !std::isfinite(s.currentPosition.GetY()) ||
                            !std::isfinite(s.previousPosition.GetY())) {
                            workerFailed.store(true, std::memory_order::release);
                            return;
                        }
                    }

                    const auto charPos = pc.BodyPosition(character);
                    if (!charPos.has_value() || !std::isfinite(static_cast<double>(charPos->GetY()))) {
                        workerFailed.store(true, std::memory_order::release);
                        return;
                    }

                    if (std::abs(pc.GetCharacterGroundOffset(character) - cachedGroundOffset) > 1e-6f) {
                        workerFailed.store(true, std::memory_order::release);
                        return;
                    }

                    if (pc.IsCharacterOnGround(character)) {
                        observedGrounded.store(true, std::memory_order::relaxed);
                    }

                    const uint32_t currentEpoch = stepEpoch.load(std::memory_order::acquire);
                    if (currentEpoch != lastSpawnedEpoch) {
                        lastSpawnedEpoch = currentEpoch;
                        for (uint32_t b = 0; b < 4u; ++b) {
                            const auto shape =
                                pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 0.2f + static_cast<float>((currentEpoch + b) & 15u) * 0.05f, 0.2f, 0.2f);
                            if (shape == nullptr) {
                                workerFailed.store(true, std::memory_order::release);
                                return;
                            }
                            const auto transient = pc.CreateRigidBody(
                                shape, JPH::RVec3(0.0, 8.0 + static_cast<double>(b), -4.0), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
                                ZHLN::Layers::ID::MOVING
                            );
                            if (transient == ZHLN::Physics::BodyHandle::Null() || !pc.IsBodyDynamic(transient)) {
                                workerFailed.store(true, std::memory_order::release);
                                return;
                            }
                            pc.AddImpulse(persistent[(currentEpoch + b) % kPersistentBodies], JPH::Vec3(0.0f, 0.25f, 0.0f));
                            pc.DestroyBody(transient);
                        }
                    }
                }
            });

            for (int step = 0; step < 30; ++step) {
                pc.SetCharacterVelocity(character, JPH::Vec3(0.5f, -2.0f, 0.0f));
                pc.Step(1.0f / 60.0f);
                stepEpoch.fetch_add(1u, std::memory_order::release);
            }

            keepRunning.store(false, std::memory_order::release);
            readerAndProducer.join();

            if (!ZHLN::Test::ExpectFalse(workerFailed.load(std::memory_order::acquire))) {
                return std::unexpected(PhysicsTestError::LockFreeConcurrencyFailed);
            }
            if (!ZHLN::Test::ExpectTrue(observedGrounded.load(std::memory_order::relaxed) || pc.IsCharacterOnGround(character))) {
                return std::unexpected(PhysicsTestError::LockFreeConcurrencyFailed);
            }

            // Drain any final transient DestroyBody queued right before join.
            pc.Step(1.0f / 60.0f);

            for (const auto h: persistent) {
                const auto state = pc.BodyState(h);
                if (!ZHLN::Test::ExpectTrue(state.has_value() && state->valid)) {
                    return std::unexpected(PhysicsTestError::LockFreeConcurrencyFailed);
                }
            }

            return {};
        }

} // namespace
} // namespace ZHLN::Test
