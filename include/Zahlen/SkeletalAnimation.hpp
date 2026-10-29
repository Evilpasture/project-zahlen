// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Math3D.hpp>
#include <cstdint>
#include <Zahlen/Core/String.hpp>
#include <vector>

namespace ZHLN {

struct AnimationHandle {
    uint32_t                  id        = InvalidID;
    static constexpr uint32_t InvalidID = ~0u;

    [[nodiscard]] static constexpr auto Null() noexcept -> AnimationHandle {
        return {};
    }

    [[nodiscard]] constexpr auto IsValid() const noexcept -> bool {
        return id != InvalidID;
    }

    constexpr auto operator==(const AnimationHandle&) const noexcept -> bool = default;
};

enum class AnimationPathType : uint8_t { Translation, Rotation, Scale, Weights };

enum class InterpolationType : uint8_t { Linear, Step, CubicSpline };

struct AnimationChannel {
    int32_t            targetNodeIndex = -1;
    AnimationPathType  path;
    InterpolationType  interpolation;
    std::vector<float> keyTimes;
    std::vector<float> keyValues;
};

struct AnimationClip {
    String64                      name;
    float                         duration = 0.0f;
    std::vector<AnimationChannel> channels;
};

struct Joint {
    String64   name;
    int32_t    parentIndex       = -1;
    int32_t    nodeIndex         = -1;
    JPH::Mat44 inverseBindMatrix = JPH::Mat44::sIdentity();
};

struct Skeleton {
    String64           name;
    std::vector<Joint> joints;
};

}
