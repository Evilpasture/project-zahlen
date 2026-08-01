// include/Zahlen/SkeletalAnimation.hpp
#pragma once
#include <Zahlen/Math3D.hpp>
#include <cstdint>
#include <Zahlen/Core/String.hpp>
#include <vector>

namespace ZHLN {

enum class AnimationPathType : uint8_t { Translation, Rotation, Scale, Weights };

enum class InterpolationType : uint8_t { Linear, Step, CubicSpline };

struct AnimationChannel {
    int32_t            targetNodeIndex = -1; // Direct index into ModelPrefab::nodes
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
    int32_t    parentIndex       = -1; // Index into the skeleton's joints array
    int32_t    nodeIndex         = -1; // Index into ModelPrefab::nodes
    JPH::Mat44 inverseBindMatrix = JPH::Mat44::sIdentity();
};

struct Skeleton {
    String64           name;
    std::vector<Joint> joints;
};

} // namespace ZHLN
