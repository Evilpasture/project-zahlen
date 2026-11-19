// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ZHLN::COM {
struct IAssetLibrary;
} // namespace ZHLN::COM

namespace ZHLN::Animation {

struct PrefabRig {
    std::string                            virtualPath;
    uint64_t                               hash     = InvalidAssetID;
    uint64_t                               revision = 0;
    std::vector<COM::PrefabNodeInfo>       nodes;
    std::vector<COM::PrefabPartBoundsInfo> parts;
    std::vector<Skeleton>                  skeletons;
    std::vector<AnimationClip>             animations;
};

[[nodiscard]] std::optional<PrefabRig> LoadPrefabRig(const COM::IAssetLibrary& assets, AssetID hash);

} // namespace ZHLN::Animation
