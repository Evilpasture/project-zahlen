// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Render/GpuEnums.hpp>

namespace ZHLN::COM {
// Phase 2 COM naming: *Info for chain and nested descriptor values;
// *View for borrowed callback payloads. AssetSlice is an offset/count helper.
struct CollisionMeshInfo {
    CollisionAssetID id = CollisionAssetID::Invalid;
    Span<const FVec3> vertices {};
    Span<const uint32_t> indices {};
};
struct EnvironmentImageCacheInfo {
    StringView key {};
    Extent2D extent {};
    Span<const float> rgba {};
    Span<const float> lightingRgba {};
    Option<EnvironmentSun> sun {};
    uint64_t contentHash = 0;
};
// All variable-length prefab data lives in flat caller spans. Slice offsets
// refer to the corresponding span in PrefabRegistrationInfo, not to byte offsets.
// Registration validates all references before copying/committing the prefab.
struct AssetSlice { uint32_t first = 0; uint32_t count = 0; };
struct PrefabNodeInfo {
    String64 name;
    int32_t parentIndex = -1;
    Mat4x4 localTransform = Mat4x4::Identity();
    bool hasMesh = false;
};
struct PrefabPartInfo {
    String64 name;
    AssetID meshAsset = InvalidAssetID;
    MaterialID materialAsset = InvalidMaterialID;
    Mesh mesh {};
    Material defaultMaterial {};
    Mat4x4 localTransform = Mat4x4::Identity();
    uint32_t jointOffset = 0;
    bool isSkinned = false;
    int32_t nodeIndex = -1;
    int32_t skeletonIndex = -1;
    uint32_t morphOffset = 0;
    uint32_t activeMorphCount = 0;
    FVec4 defaultMorphWeights {};
    float boundingRadius = 1.0f;
    FVec3 localMin {};
    FVec3 localMax {};
    CollisionAssetID collision = CollisionAssetID::Invalid;
    bool hasBoxCollider = false;
    FVec3 boxHalfExtents {0.5f, 0.5f, 0.5f};
    FVec3 boxCenter {};
    // Some importers intentionally use the same simplified box for both the
    // static/mesh and dynamic/box collider choices. Preserve that shape kind
    // without a native Physics::AnyShape or a triangulated-box approximation.
    bool useBoxForMeshCollider = false;
    // Optional second mesh child, e.g. BSP invisible-but-solid tool faces.
    // Combined with the selected mesh/box fallback as an engine-owned compound;
    // does not enlarge or replace the independent box-collider choice.
    CollisionAssetID additionalMeshCollision = CollisionAssetID::Invalid;
    DrawFlags drawFlags = DrawFlags::None;
    AssetSlice csgModifiers {};
};
struct PrefabJointInfo {
    String64 name;
    int32_t parentIndex = -1;
    int32_t nodeIndex = -1;
    Mat4x4 inverseBindMatrix = Mat4x4::Identity();
};
struct PrefabSkeletonInfo { String64 name; AssetSlice joints {}; };
struct PrefabChannelInfo {
    int32_t targetNodeIndex = -1;
    AnimationPathType path = AnimationPathType::Translation;
    InterpolationType interpolation = InterpolationType::Linear;
    AssetSlice keyTimes {};
    // Cubic TRS channels carry tangent/value/tangent triples. Weight channels
    // retain the importer's compact value-only playback representation.
    AssetSlice keyValues {};
};
struct PrefabAnimationInfo { String64 name; float duration = 0.0f; AssetSlice channels {}; };
struct PrefabLightInfo {
    String64 name;
    int32_t nodeIndex = -1;
    LightType type = LightType::Point;
    FVec3 color {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    float range = 0.0f;
    float innerConeAngle = 0.0f;
    float outerConeAngle = 0.78539816339f;
};
struct PrefabCSGModifierInfo { CSGOperation operation {}; String64 operandName; };
struct PrefabRegistrationInfo {
    StringView virtualPath {};
    float emissiveFactorScale = 100.0f;
    uint32_t maxTextureDimension = 1024;
    Span<const PrefabNodeInfo> nodes {};
    Span<const PrefabPartInfo> parts {};
    Span<const PrefabSkeletonInfo> skeletons {};
    Span<const PrefabJointInfo> joints {};
    Span<const PrefabAnimationInfo> animations {};
    Span<const PrefabChannelInfo> channels {};
    Span<const float> keyTimes {};
    Span<const float> keyValues {};
    Span<const PrefabLightInfo> lights {};
    Span<const PrefabCSGModifierInfo> csgModifiers {};
};
template <typename T> inline constexpr bool AssetWireValue = std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>;
static_assert(AssetWireValue<PrefabCSGModifierInfo>);
static_assert(AssetWireValue<CollisionMeshInfo> && AssetWireValue<EnvironmentImageCacheInfo>);
static_assert(AssetWireValue<AssetSlice> && AssetWireValue<PrefabNodeInfo> && AssetWireValue<PrefabPartInfo>);
static_assert(AssetWireValue<PrefabJointInfo> && AssetWireValue<PrefabSkeletonInfo> && AssetWireValue<PrefabChannelInfo>);
static_assert(AssetWireValue<PrefabAnimationInfo> && AssetWireValue<PrefabLightInfo> && AssetWireValue<PrefabRegistrationInfo>);
}
