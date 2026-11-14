// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Animation/ProceduralMath.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <cstddef>

namespace ZHLN::Animation {

using AttachmentQuery = ECS::Query<const Components::MeshComponent, const Components::HierarchyComponent,
                                   const Components::SkeletalMeshComponent, Components::TransformComponent&>;

namespace AttachmentMath {
[[nodiscard]] inline FVec3 ExtractScale(const Mat4x4& matrix) noexcept {
    return {ProceduralMath::Column3(matrix, 0).Length(), ProceduralMath::Column3(matrix, 1).Length(), ProceduralMath::Column3(matrix, 2).Length()};
}
[[nodiscard]] inline Quat ExtractRotation(const Mat4x4& matrix) noexcept {
    const FVec3 scale = ExtractScale(matrix);
    const FVec3 x = scale.x > 1.0e-6f ? ProceduralMath::Column3(matrix, 0) / scale.x : FVec3::UnitX();
    const FVec3 y = scale.y > 1.0e-6f ? ProceduralMath::Column3(matrix, 1) / scale.y : FVec3::UnitY();
    const FVec3 z = scale.z > 1.0e-6f ? ProceduralMath::Column3(matrix, 2) / scale.z : FVec3::UnitZ();
    return ProceduralMath::Rotation(Mat4x4::FromCols(FVec4{x, 0.0f}, FVec4{y, 0.0f}, FVec4{z, 0.0f}, FVec4{0.0f, 0.0f, 0.0f, 1.0f}));
}
} // namespace AttachmentMath

// The rig map is a module-defined type. Keeping this narrow helper generic
// avoids importing that module from its own implementation unit. Callers own
// phase isolation; the query exposes only the families this pass touches.
template <typename RigMap>
size_t SyncNonSkinnedAttachments(AttachmentQuery query, Entity rootEntity, const RigMap& boneMap) noexcept {
    size_t count = 0;
    for (Entity child: query.Entities<Components::MeshComponent>()) {
        const auto hierarchy = query.Get<Components::HierarchyComponent>(child);
        if (!hierarchy || hierarchy->parent != rootEntity) continue;
        const auto mesh = query.Get<Components::MeshComponent>(child);
        if (!mesh || mesh->nodeIndex < 0) continue;
        const size_t node = static_cast<size_t>(mesh->nodeIndex);
        if (node >= boneMap.nodeCount) continue;
        const auto skeletal = query.Get<Components::SkeletalMeshComponent>(child);
        if (skeletal && skeletal->skeletonIndex >= 0) continue;
        if (auto transform = query.Get<Components::TransformComponent>(child)) {
            const Mat4x4& model = boneMap.modelTransforms[node];
            transform->position = ProceduralMath::Translation(model);
            transform->rotation = AttachmentMath::ExtractRotation(model);
            transform->scale = AttachmentMath::ExtractScale(model);
            ++count;
        }
    }
    return count;
}

} // namespace ZHLN::Animation
