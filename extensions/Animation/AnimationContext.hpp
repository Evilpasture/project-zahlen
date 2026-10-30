// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Animation/AnimationContext.hpp
//
// Monolithic skeletal animation domain object, peer in shape to PhysicsContext,
// AudioContext, and RenderContext: generational handles in, dense parallel pose
// evaluation inside, value/span/Optional queries out. It knows nothing about
// ECS::Registry or Entity; stateless ECS systems translate components to and
// from this interface.
#pragma once

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <cstdint>
#include <memory>
#include <span>

namespace ZHLN {

class AssetManager;
class PoseUploadQueue;

namespace Animation {

struct PlaybackConfig {
    AssetID prefabAsset   = InvalidAssetID;
    int32_t trackIndex    = -1;
    float   trackTime     = 0.0f;
    float   playbackSpeed = 1.0f;
    float   blendDuration = 0.15f;
    bool    loop          = true;
    bool    finished      = false;
};

struct PlaybackState {
    float trackTime = 0.0f;
    bool  finished  = false;
};

struct MorphSample {
    uint32_t    activeCount = 0;
    JPH::Float4 weights     = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct SkeletonBinding {
    uint32_t skeletonIndex = 0;
    uint32_t jointOffset   = 0;
    bool     hasRagdoll    = false;
};

struct IKChain {
    int32_t   upperNodeIndex    = -1;
    int32_t   lowerNodeIndex    = -1;
    int32_t   endNodeIndex      = -1;
    JPH::Vec3 targetPosition    = JPH::Vec3::sZero();
    JPH::Quat targetRotation    = JPH::Quat::sIdentity();
    JPH::Vec3 poleVector        = JPH::Vec3(0.0f, -1.0f, 0.0f);
    float     weight            = 1.0f;
    bool      orientEndEffector = true;
};

class AnimationContext {
  public:
    AnimationContext();
    ~AnimationContext() noexcept;

    AnimationContext(const AnimationContext&)                    = delete;
    auto operator=(const AnimationContext&) -> AnimationContext& = delete;
    AnimationContext(AnimationContext&&) noexcept;
    auto operator=(AnimationContext&&) noexcept -> AnimationContext&;

    [[nodiscard]] auto Create(AssetID prefabAsset) -> AnimationHandle;
    void               Destroy(std::span<const AnimationHandle> handles) noexcept;
    void               Clear() noexcept;
    [[nodiscard]] auto Valid(AnimationHandle handle) const noexcept -> bool;
    [[nodiscard]] auto ActiveCount() const noexcept -> size_t;

    void Sync(AnimationHandle handle, const PlaybackConfig& config) noexcept;
    void BindSkeletons(AnimationHandle handle, std::span<const SkeletonBinding> bindings) noexcept;
    void SetIKChains(AnimationHandle handle, std::span<const IKChain> chains) noexcept;
    void SuppressPaletteUpload(AnimationHandle handle) noexcept;

    void Step(float dt, const AssetManager& assets, PoseUploadQueue& poseUploads);

    [[nodiscard]] auto Playback(AnimationHandle handle) const noexcept -> ZHLN::Optional<PlaybackState>;
    [[nodiscard]] auto Morph(AnimationHandle handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<MorphSample>;
    [[nodiscard]] auto RigidNode(AnimationHandle handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<Math::TransformTRS>;
    [[nodiscard]] auto LocalPose(AnimationHandle handle) const noexcept -> std::span<const JPH::Mat44>;
    [[nodiscard]] auto ModelPose(AnimationHandle handle) const noexcept -> std::span<const JPH::Mat44>;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Animation
} // namespace ZHLN
