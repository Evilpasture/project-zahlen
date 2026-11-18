// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Animation/AnimationID.hpp>
#include <Animation/PrefabRig.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <cstdint>
#include <memory>
#include <span>

namespace ZHLN {
namespace COM {
struct IAssetLibrary;
struct IDebugDraw;
struct IEngine;
struct IEntities;
struct IGraphicsDevice;
struct ISimulation;
} // namespace COM

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
    ZHLN::FVec4 weights     = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct SkeletonBinding {
    uint32_t skeletonIndex = 0;
    uint32_t jointOffset   = 0;
    bool     hasRagdoll    = false;
};

struct IKChain {
    int32_t upperNodeIndex    = -1;
    int32_t lowerNodeIndex    = -1;
    int32_t endNodeIndex      = -1;
    FVec3   targetPosition    = FVec3::Zero();
    Quat    targetRotation    = Quat::Identity();
    FVec3   poleVector        = {0.0f, -1.0f, 0.0f};
    float   weight            = 1.0f;
    bool    orientEndEffector = true;
};

struct PoseUploadCallback {
    void* user = nullptr;
    void (*uploadPalette)(void* user, uint32_t jointOffset, std::span<const Mat4x4> joints)   = nullptr;
    void (*uploadKinematic)(void* user, uint32_t jointOffset, std::span<const Mat4x4> joints) = nullptr;
};

class ZHLN_STATIC_API AnimationContext {
  public:
    AnimationContext();
    ~AnimationContext() noexcept;

    AnimationContext(const AnimationContext&)                    = delete;
    auto operator=(const AnimationContext&) -> AnimationContext& = delete;
    AnimationContext(AnimationContext&&) noexcept;
    auto operator=(AnimationContext&&) noexcept -> AnimationContext&;

    [[nodiscard]] auto Create(AssetID prefabAsset) -> AnimationID;
    void               Destroy(std::span<const AnimationID> handles) noexcept;
    void               Clear() noexcept;
    [[nodiscard]] auto Valid(AnimationID handle) const noexcept -> bool;
    [[nodiscard]] auto ActiveCount() const noexcept -> size_t;
    void               Manage(AnimationID handle);
    void               CollectManaged(std::span<const AnimationID> retained);

    void Sync(AnimationID handle, const PlaybackConfig& config) noexcept;
    void BindSkeletons(AnimationID handle, std::span<const SkeletonBinding> bindings) noexcept;
    void SetIKChains(AnimationID handle, std::span<const IKChain> chains) noexcept;
    void SuppressPaletteUpload(AnimationID handle) noexcept;

    void                           CacheRig(AssetID id, PrefabRig rig);
    void                           InvalidateRigs() noexcept;
    [[nodiscard]] auto             ResolveRig(const COM::IAssetLibrary* assets, AssetID id) -> const PrefabRig*;

    void Step(
        float                     dt,
        const COM::IAssetLibrary* assets     = nullptr,
        COM::IGraphicsDevice*     graphics   = nullptr,
        COM::ISimulation*         simulation = nullptr,
        PoseUploadCallback        sink       = {});

    [[nodiscard]] auto Playback(AnimationID handle) const noexcept -> ZHLN::Optional<PlaybackState>;
    [[nodiscard]] auto Morph(AnimationID handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<MorphSample>;
    [[nodiscard]] auto RigidNode(AnimationID handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<TransformParts>;
    [[nodiscard]] auto LocalPose(AnimationID handle) const noexcept -> std::span<const Mat4x4>;
    [[nodiscard]] auto ModelPose(AnimationID handle) const noexcept -> std::span<const Mat4x4>;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace Animation
} // namespace ZHLN
