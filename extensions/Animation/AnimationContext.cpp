// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AnimationContext.hpp"
#include "IK.hpp"
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace ZHLN::Animation {

namespace {

constexpr uint32_t kIndexBits     = 20u;
constexpr uint32_t kIndexMask     = (1u << kIndexBits) - 1u;
constexpr uint16_t kMinGeneration = 1u;
constexpr uint16_t kMaxGeneration = 0x0FFEu;
constexpr uint32_t kInvalidDense  = std::numeric_limits<uint32_t>::max();

constexpr size_t kMaxBoundSkeletons = 8;
constexpr size_t kMaxIKChains       = 4;

[[nodiscard]] constexpr auto PackHandle(uint32_t slotIndex, uint16_t generation) noexcept -> AnimationHandle {
    return AnimationHandle {.id = (static_cast<uint32_t>(generation) << kIndexBits) | (slotIndex & kIndexMask)};
}

[[nodiscard]] constexpr auto UnpackSlot(AnimationHandle handle) noexcept -> uint32_t {
    return handle.id & kIndexMask;
}

[[nodiscard]] constexpr auto UnpackGeneration(AnimationHandle handle) noexcept -> uint16_t {
    return static_cast<uint16_t>(handle.id >> kIndexBits);
}

[[nodiscard]] constexpr auto SmoothFactor(InterpolationType interpolation, float rawFactor) noexcept -> float {
    if (interpolation == InterpolationType::Step) {
        return 0.0f;
    }
    const float t = std::clamp(rawFactor, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

struct SampledTRS {
    JPH::Vec3 translation = JPH::Vec3::sZero();
    JPH::Quat rotation    = JPH::Quat::sIdentity();
    JPH::Vec3 scale       = JPH::Vec3::sReplicate(1.0f);
};

void SampleTRSChannel(const AnimationChannel& channel, float time, SampledTRS& target) noexcept {
    if (channel.keyTimes.empty()) {
        return;
    }

    const auto   it     = std::ranges::upper_bound(channel.keyTimes, time);
    const size_t idx1   = (it == channel.keyTimes.end()) ? channel.keyTimes.size() - 1 : static_cast<size_t>(std::distance(channel.keyTimes.begin(), it));
    const size_t idx0   = (idx1 > 0) ? idx1 - 1 : 0;
    const float  t0     = channel.keyTimes[idx0];
    const float  t1     = channel.keyTimes[idx1];
    const float  raw    = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
    const float  factor = SmoothFactor(channel.interpolation, raw);

    if (channel.path == AnimationPathType::Translation) {
        if (channel.keyValues.size() < (idx1 + 1) * 3) {
            return;
        }
        const float* v0    = &channel.keyValues[idx0 * 3];
        const float* v1    = &channel.keyValues[idx1 * 3];
        target.translation = JPH::Vec3(v0[0] + factor * (v1[0] - v0[0]), v0[1] + factor * (v1[1] - v0[1]), v0[2] + factor * (v1[2] - v0[2]));
    } else if (channel.path == AnimationPathType::Rotation) {
        if (channel.keyValues.size() < (idx1 + 1) * 4) {
            return;
        }
        const float*    q0 = &channel.keyValues[idx0 * 4];
        const float*    q1 = &channel.keyValues[idx1 * 4];
        const JPH::Quat rot0(q0[0], q0[1], q0[2], q0[3]);
        const JPH::Quat rot1(q1[0], q1[1], q1[2], q1[3]);
        target.rotation = rot0.SLERP(rot1, factor).Normalized();
    } else if (channel.path == AnimationPathType::Scale) {
        if (channel.keyValues.size() < (idx1 + 1) * 3) {
            return;
        }
        const float* s0 = &channel.keyValues[idx0 * 3];
        const float* s1 = &channel.keyValues[idx1 * 3];
        target.scale    = JPH::Vec3(s0[0] + factor * (s1[0] - s0[0]), s0[1] + factor * (s1[1] - s0[1]), s0[2] + factor * (s1[2] - s0[2]));
    }
}

[[nodiscard]] auto SampleMorphChannel(const AnimationChannel& channel, float time) noexcept -> MorphSample {
    if (channel.keyTimes.empty()) {
        return {};
    }

    const auto numWeights = static_cast<uint32_t>(channel.keyValues.size() / channel.keyTimes.size());
    if (numWeights == 0) {
        return {};
    }

    const auto   it     = std::ranges::upper_bound(channel.keyTimes, time);
    const size_t idx1   = (it == channel.keyTimes.end()) ? channel.keyTimes.size() - 1 : static_cast<size_t>(std::distance(channel.keyTimes.begin(), it));
    const size_t idx0   = (idx1 > 0) ? idx1 - 1 : 0;
    const float  t0     = channel.keyTimes[idx0];
    const float  t1     = channel.keyTimes[idx1];
    const float  raw    = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;
    const float  factor = SmoothFactor(channel.interpolation, raw);

    const uint32_t           count = std::min(numWeights, 4u);
    const float*             v0    = &channel.keyValues[idx0 * numWeights];
    const float*             v1    = &channel.keyValues[idx1 * numWeights];
    std::array<float, 4>     lanes {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t w = 0; w < count; ++w) {
        lanes[w] = v0[w] + factor * (v1[w] - v0[w]);
    }
    return MorphSample {
        .activeCount = count,
        .weights     = JPH::Float4 {lanes[0], lanes[1], lanes[2], lanes[3]},
    };
}

struct NodeEvalScratch {
    SampledTRS prev {};
    SampledTRS curr {};
    uint8_t    fkState        = 0; // 0 = unvisited, 1 = visiting, 2 = computed
    bool       directlySolved = false;
    bool       subtreeDirty   = false;
};

struct RigidNodeSlot {
    Math::TransformTRS trs {};
    bool               valid = false;
};

struct FreeRange {
    uint32_t offset = 0;
    uint32_t count  = 0;
};

class PosePool {
  public:
    [[nodiscard]] auto Allocate(uint32_t count) -> uint32_t {
        if (count == 0) {
            return 0;
        }
        for (size_t i = 0; i < _free.size(); ++i) {
            if (_free[i].count >= count) {
                const uint32_t offset = _free[i].offset;
                if (_free[i].count == count) {
                    _free.erase(_free.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    _free[i].offset += count;
                    _free[i].count -= count;
                }
                ResetRange(offset, count);
                return offset;
            }
        }

        const auto offset  = static_cast<uint32_t>(_local.size());
        const auto newSize = static_cast<size_t>(offset) + static_cast<size_t>(count);
        _clipLocal.resize(newSize, JPH::Mat44::sIdentity());
        _local.resize(newSize, JPH::Mat44::sIdentity());
        _model.resize(newSize, JPH::Mat44::sIdentity());
        _morphs.resize(newSize);
        _rigidNodes.resize(newSize);
        _scratch.resize(newSize);
        return offset;
    }

    void Free(uint32_t offset, uint32_t count) noexcept {
        if (count == 0) {
            return;
        }
        const auto insertPos = std::ranges::lower_bound(_free, offset, {}, &FreeRange::offset);
        auto       it        = _free.insert(insertPos, FreeRange {.offset = offset, .count = count});

        if (it != _free.begin()) {
            const auto prev = std::prev(it);
            if (prev->offset + prev->count == it->offset) {
                prev->count += it->count;
                it = _free.erase(it);
            }
        }
        const auto next = std::next(it);
        if (next != _free.end() && it->offset + it->count == next->offset) {
            it->count += next->count;
            _free.erase(next);
        }
    }

    void Clear() noexcept {
        _clipLocal.clear();
        _local.clear();
        _model.clear();
        _morphs.clear();
        _rigidNodes.clear();
        _scratch.clear();
        _free.clear();
    }

    [[nodiscard]] auto ClipLocal(uint32_t offset, uint32_t count) noexcept -> std::span<JPH::Mat44> {
        return std::span<JPH::Mat44> {_clipLocal.data() + offset, count};
    }
    [[nodiscard]] auto Local(uint32_t offset, uint32_t count) noexcept -> std::span<JPH::Mat44> {
        return std::span<JPH::Mat44> {_local.data() + offset, count};
    }
    [[nodiscard]] auto Local(uint32_t offset, uint32_t count) const noexcept -> std::span<const JPH::Mat44> {
        return std::span<const JPH::Mat44> {_local.data() + offset, count};
    }
    [[nodiscard]] auto Model(uint32_t offset, uint32_t count) noexcept -> std::span<JPH::Mat44> {
        return std::span<JPH::Mat44> {_model.data() + offset, count};
    }
    [[nodiscard]] auto Model(uint32_t offset, uint32_t count) const noexcept -> std::span<const JPH::Mat44> {
        return std::span<const JPH::Mat44> {_model.data() + offset, count};
    }
    [[nodiscard]] auto Morphs(uint32_t offset, uint32_t count) noexcept -> std::span<MorphSample> {
        return std::span<MorphSample> {_morphs.data() + offset, count};
    }
    [[nodiscard]] auto Morphs(uint32_t offset, uint32_t count) const noexcept -> std::span<const MorphSample> {
        return std::span<const MorphSample> {_morphs.data() + offset, count};
    }
    [[nodiscard]] auto RigidNodes(uint32_t offset, uint32_t count) noexcept -> std::span<RigidNodeSlot> {
        return std::span<RigidNodeSlot> {_rigidNodes.data() + offset, count};
    }
    [[nodiscard]] auto RigidNodes(uint32_t offset, uint32_t count) const noexcept -> std::span<const RigidNodeSlot> {
        return std::span<const RigidNodeSlot> {_rigidNodes.data() + offset, count};
    }
    [[nodiscard]] auto Scratch(uint32_t offset, uint32_t count) noexcept -> std::span<NodeEvalScratch> {
        return std::span<NodeEvalScratch> {_scratch.data() + offset, count};
    }

  private:
    void ResetRange(uint32_t offset, uint32_t count) noexcept {
        std::fill_n(_clipLocal.begin() + offset, count, JPH::Mat44::sIdentity());
        std::fill_n(_local.begin() + offset, count, JPH::Mat44::sIdentity());
        std::fill_n(_model.begin() + offset, count, JPH::Mat44::sIdentity());
        std::fill_n(_morphs.begin() + offset, count, MorphSample {});
        std::fill_n(_rigidNodes.begin() + offset, count, RigidNodeSlot {});
        std::fill_n(_scratch.begin() + offset, count, NodeEvalScratch {});
    }

    std::vector<JPH::Mat44>      _clipLocal;
    std::vector<JPH::Mat44>      _local;
    std::vector<JPH::Mat44>      _model;
    std::vector<MorphSample>     _morphs;
    std::vector<RigidNodeSlot>   _rigidNodes;
    std::vector<NodeEvalScratch> _scratch;
    std::vector<FreeRange>       _free;
};

struct Slot {
    uint32_t denseIndex = kInvalidDense;
    uint16_t generation = kMinGeneration;
};

struct UploadSlice {
    uint32_t jointOffset     = 0;
    uint32_t paletteOffset   = 0;
    uint32_t kinematicOffset = 0;
    uint32_t jointCount      = 0;
    uint32_t skeletonIndex   = 0;
    bool     hasRagdoll      = false;
};

struct InstanceState {
    AssetID prefabAsset = InvalidAssetID;

    int32_t activeTrack       = -1;
    float   activeTime        = 0.0f;
    float   activeSpeed       = 1.0f;
    int32_t prevTrack         = -1;
    float   prevTime          = 0.0f;
    float   prevSpeed         = 1.0f;
    float   blendFactor       = 1.0f;
    float   blendDuration     = 0.15f;
    float   lastPublishedTime = 0.0f;
    bool    loop              = true;
    bool    finished          = false;
    bool    initialized       = false;
    bool    dirtyPose         = true;
    bool    hadIKLastStep     = false;

    uint32_t poseOffset = 0;
    uint32_t nodeCount  = 0;

    std::array<SkeletonBinding, kMaxBoundSkeletons> skeletons {};
    uint32_t                                        skeletonCount = 0;

    std::array<IKChain, kMaxIKChains> ikChains {};
    uint32_t                          ikChainCount = 0;

    std::array<UploadSlice, kMaxBoundSkeletons> uploadSlices {};
    uint32_t                                    uploadSliceCount      = 0;
    bool                                        suppressPaletteUpload = false;
};

void ResolveBaseFK(const ModelPrefab& prefab, std::span<const JPH::Mat44> local, std::span<JPH::Mat44> model, std::span<NodeEvalScratch> scratch) noexcept {
    const size_t nodeCount = local.size();
    for (size_t n = 0; n < nodeCount; ++n) {
        scratch[n].fkState        = 0;
        scratch[n].directlySolved = false;
        scratch[n].subtreeDirty   = false;
    }

    auto resolve = [&](auto& self, int32_t idx) -> void {
        if (idx < 0 || static_cast<size_t>(idx) >= nodeCount) {
            return;
        }
        const auto       node  = static_cast<size_t>(idx);
        NodeEvalScratch& state = scratch[node];
        if (state.fkState == 2) {
            return;
        }
        if (state.fkState == 1) {
            model[node]   = local[node];
            state.fkState = 2;
            return;
        }
        state.fkState        = 1;
        const int32_t parent = prefab.nodes[node].parentIndex;
        if (parent >= 0 && static_cast<size_t>(parent) < nodeCount) {
            self(self, parent);
            model[node] = model[static_cast<size_t>(parent)] * local[node];
        } else {
            model[node] = local[node];
        }
        state.fkState = 2;
    };

    for (size_t n = 0; n < nodeCount; ++n) {
        resolve(resolve, static_cast<int32_t>(n));
    }
}

void ApplyIKAndPropagateDescendantFK(
    const ModelPrefab&     prefab,
    std::span<const IKChain> chains,
    std::span<JPH::Mat44>  local,
    std::span<JPH::Mat44>  model,
    std::span<NodeEvalScratch> scratch
) noexcept {
    const size_t nodeCount = local.size();
    bool         anySolved = false;

    for (const IKChain& chain: chains) {
        if (chain.weight <= 0.001f || chain.upperNodeIndex < 0 || chain.lowerNodeIndex < 0 || chain.endNodeIndex < 0) {
            continue;
        }
        if (static_cast<size_t>(chain.upperNodeIndex) >= nodeCount || static_cast<size_t>(chain.lowerNodeIndex) >= nodeCount ||
            static_cast<size_t>(chain.endNodeIndex) >= nodeCount) {
            continue;
        }

        const auto       upperIdx   = static_cast<size_t>(chain.upperNodeIndex);
        const auto       lowerIdx   = static_cast<size_t>(chain.lowerNodeIndex);
        const auto       endIdx     = static_cast<size_t>(chain.endNodeIndex);
        const JPH::Mat44 upperModel = model[upperIdx];
        const JPH::Mat44 lowerModel = model[lowerIdx];
        const JPH::Mat44 endModel   = model[endIdx];

        const JPH::Vec3 pUpper = upperModel.GetTranslation();
        const JPH::Vec3 pLower = lowerModel.GetTranslation();
        const JPH::Vec3 pEnd   = endModel.GetTranslation();

        const IK::TwoBoneIKSolverOutput solved = IK::SolveTwoBoneIK(
            IK::TwoBoneIKSolverInput {
                .upperPosition  = pUpper,
                .targetPosition = chain.targetPosition,
                .poleVector     = chain.poleVector,
                .upperLength    = (pLower - pUpper).Length(),
                .lowerLength    = (pEnd - pLower).Length(),
            }
        );
        if (!solved.valid) {
            continue;
        }

        const JPH::Vec3 localUpperVec = local[lowerIdx].GetTranslation();
        const JPH::Vec3 localLowerVec = local[endIdx].GetTranslation();
        if (localUpperVec.LengthSq() <= 1.0e-10f || localLowerVec.LengthSq() <= 1.0e-10f) {
            continue;
        }

        const JPH::Mat44 newUpper = IK::AlignNodeToDirection(upperModel, localUpperVec.Normalized(), solved.upperDirection);
        JPH::Mat44       newLower = JPH::Mat44::sRotationTranslation(lowerModel.GetQuaternion().Normalized(), solved.midPosition);
        newLower                  = IK::AlignNodeToDirection(newLower, localLowerVec.Normalized(), solved.lowerDirection);

        JPH::Mat44 newEnd = endModel;
        newEnd.SetTranslation(solved.endPosition);
        if (chain.orientEndEffector) {
            newEnd = JPH::Mat44::sRotationTranslation(chain.targetRotation.Normalized(), solved.endPosition);
        }

        const float w     = std::clamp(chain.weight, 0.0f, 1.0f);
        auto        blend = [](const JPH::Mat44& a, const JPH::Mat44& b, float t) noexcept -> JPH::Mat44 {
            const JPH::Vec3 tA = a.GetTranslation();
            const JPH::Vec3 tB = b.GetTranslation();
            const JPH::Quat rA = a.GetQuaternion().Normalized();
            const JPH::Quat rB = b.GetQuaternion().Normalized();
            return JPH::Mat44::sRotationTranslation(rA.SLERP(rB, t).Normalized(), tA + t * (tB - tA));
        };

        model[upperIdx] = blend(upperModel, newUpper, w);
        model[lowerIdx] = blend(lowerModel, newLower, w);
        model[endIdx]   = blend(endModel, newEnd, w);

        scratch[upperIdx].directlySolved = true;
        scratch[lowerIdx].directlySolved = true;
        scratch[endIdx].directlySolved   = true;
        scratch[upperIdx].subtreeDirty   = true;
        scratch[lowerIdx].subtreeDirty   = true;
        scratch[endIdx].subtreeDirty     = true;
        anySolved                        = true;
    }

    if (!anySolved) {
        return;
    }

    for (size_t n = 0; n < nodeCount; ++n) {
        scratch[n].fkState = scratch[n].directlySolved ? 2u : 0u;
    }

    auto propagate = [&](auto& self, int32_t idx) -> void {
        if (idx < 0 || static_cast<size_t>(idx) >= nodeCount) {
            return;
        }
        const auto       node  = static_cast<size_t>(idx);
        NodeEvalScratch& state = scratch[node];
        if (state.fkState == 2) {
            return;
        }
        if (state.fkState == 1) {
            state.fkState = 2;
            return;
        }
        state.fkState        = 1;
        const int32_t parent = prefab.nodes[node].parentIndex;
        if (parent >= 0 && static_cast<size_t>(parent) < nodeCount) {
            self(self, parent);
            if (scratch[static_cast<size_t>(parent)].subtreeDirty) {
                model[node]        = model[static_cast<size_t>(parent)] * local[node];
                state.subtreeDirty = true;
            }
        }
        state.fkState = 2;
    };

    for (size_t n = 0; n < nodeCount; ++n) {
        propagate(propagate, static_cast<int32_t>(n));
    }

    for (size_t n = 0; n < nodeCount; ++n) {
        if (!scratch[n].directlySolved) {
            continue;
        }
        const int32_t parent = prefab.nodes[n].parentIndex;
        local[n]             = (parent >= 0 && static_cast<size_t>(parent) < nodeCount) ?
                                   model[static_cast<size_t>(parent)].Inversed() * model[n] :
                                   model[n];
    }
}

} // namespace

struct AnimationContext::Impl {
    std::vector<Slot>            slots;
    std::vector<uint32_t>        freeSlots;
    std::vector<AnimationHandle> activeHandles;
    std::vector<InstanceState>   instances;
    PosePool                     poses;

    std::vector<const ModelPrefab*> stepPrefabs;
    std::vector<JPH::Mat44>         paletteScratch;
    std::vector<JPH::Mat44>         kinematicScratch;

    [[nodiscard]] auto ResolveDense(AnimationHandle handle) const noexcept -> ZHLN::Optional<uint32_t> {
        if (!handle.IsValid()) {
            return std::nullopt;
        }
        const uint32_t slotIdx = UnpackSlot(handle);
        if (slotIdx >= slots.size()) {
            return std::nullopt;
        }
        const Slot& slot = slots[slotIdx];
        if (slot.denseIndex == kInvalidDense || slot.generation != UnpackGeneration(handle)) {
            return std::nullopt;
        }
        return slot.denseIndex;
    }
};

AnimationContext::AnimationContext(): _impl(std::make_unique<Impl>()) {
}

AnimationContext::~AnimationContext() noexcept = default;

AnimationContext::AnimationContext(AnimationContext&&) noexcept = default;

auto AnimationContext::operator=(AnimationContext&&) noexcept -> AnimationContext& = default;

auto AnimationContext::Create(AssetID prefabAsset) -> AnimationHandle {
    uint32_t slotIdx = 0;
    if (!_impl->freeSlots.empty()) {
        slotIdx = _impl->freeSlots.back();
        _impl->freeSlots.pop_back();
    } else {
        slotIdx = static_cast<uint32_t>(_impl->slots.size());
        _impl->slots.push_back(Slot {});
    }

    const auto            denseIdx = static_cast<uint32_t>(_impl->instances.size());
    Slot&                 slot     = _impl->slots[slotIdx];
    slot.denseIndex                = denseIdx;
    const AnimationHandle handle   = PackHandle(slotIdx, slot.generation);

    _impl->activeHandles.push_back(handle);
    _impl->instances.push_back(InstanceState {.prefabAsset = prefabAsset});
    return handle;
}

void AnimationContext::Destroy(std::span<const AnimationHandle> handles) noexcept {
    for (const AnimationHandle handle: handles) {
        const auto denseOpt = _impl->ResolveDense(handle);
        if (!denseOpt) {
            continue;
        }
        const uint32_t denseIdx = *denseOpt;
        const uint32_t slotIdx  = UnpackSlot(handle);

        InstanceState& doomed = _impl->instances[denseIdx];
        _impl->poses.Free(doomed.poseOffset, doomed.nodeCount);

        const auto lastDense = static_cast<uint32_t>(_impl->instances.size() - 1);
        if (denseIdx != lastDense) {
            const AnimationHandle movedHandle            = _impl->activeHandles[lastDense];
            _impl->activeHandles[denseIdx]               = movedHandle;
            _impl->instances[denseIdx]                   = _impl->instances[lastDense];
            _impl->slots[UnpackSlot(movedHandle)].denseIndex = denseIdx;
        }
        _impl->activeHandles.pop_back();
        _impl->instances.pop_back();

        Slot& slot      = _impl->slots[slotIdx];
        slot.denseIndex = kInvalidDense;
        slot.generation = (slot.generation >= kMaxGeneration) ? kMinGeneration : static_cast<uint16_t>(slot.generation + 1u);
        _impl->freeSlots.push_back(slotIdx);
    }
}

void AnimationContext::Clear() noexcept {
    for (uint32_t slotIdx = 0; slotIdx < static_cast<uint32_t>(_impl->slots.size()); ++slotIdx) {
        Slot& slot = _impl->slots[slotIdx];
        if (slot.denseIndex != kInvalidDense) {
            slot.denseIndex = kInvalidDense;
            slot.generation = (slot.generation >= kMaxGeneration) ? kMinGeneration : static_cast<uint16_t>(slot.generation + 1u);
            _impl->freeSlots.push_back(slotIdx);
        }
    }
    _impl->activeHandles.clear();
    _impl->instances.clear();
    _impl->poses.Clear();
}

auto AnimationContext::Valid(AnimationHandle handle) const noexcept -> bool {
    return _impl->ResolveDense(handle).has_value();
}

auto AnimationContext::ActiveCount() const noexcept -> size_t {
    return _impl->instances.size();
}

void AnimationContext::Sync(AnimationHandle handle, const PlaybackConfig& config) noexcept {
    const auto denseOpt = _impl->ResolveDense(handle);
    if (!denseOpt) {
        return;
    }
    InstanceState& inst = _impl->instances[*denseOpt];

    if (inst.prefabAsset != config.prefabAsset) {
        inst.prefabAsset = config.prefabAsset;
        inst.dirtyPose   = true;
    }

    inst.blendDuration = config.blendDuration;
    if (inst.loop != config.loop) {
        inst.loop      = config.loop;
        inst.dirtyPose = true;
    }

    if (!inst.initialized) {
        inst.activeTrack       = config.trackIndex;
        inst.activeTime        = config.trackTime;
        inst.activeSpeed       = config.playbackSpeed;
        inst.lastPublishedTime = config.trackTime;
        inst.finished          = config.finished;
        inst.blendFactor       = 1.0f;
        inst.prevTrack         = -1;
        inst.initialized       = true;
        inst.dirtyPose         = true;
        return;
    }

    if (config.trackIndex != inst.activeTrack) {
        if (config.trackIndex >= 0 && config.trackIndex == inst.prevTrack && inst.blendFactor < 1.0f) {
            std::swap(inst.prevTrack, inst.activeTrack);
            std::swap(inst.prevTime, inst.activeTime);
            std::swap(inst.prevSpeed, inst.activeSpeed);
            inst.activeSpeed       = config.playbackSpeed;
            inst.blendFactor       = 1.0f - inst.blendFactor;
            inst.finished          = false;
            inst.lastPublishedTime = inst.activeTime;
        } else {
            inst.prevTrack         = inst.activeTrack;
            inst.prevTime          = inst.activeTime;
            inst.prevSpeed         = inst.activeSpeed;
            inst.activeTrack       = config.trackIndex;
            inst.activeTime        = config.trackTime;
            inst.activeSpeed       = config.playbackSpeed;
            inst.blendFactor       = (inst.prevTrack >= 0 && config.trackIndex >= 0 && inst.blendDuration > 0.0f) ? 0.0f : 1.0f;
            inst.finished          = false;
            inst.lastPublishedTime = config.trackTime;
        }
        inst.dirtyPose = true;
        return;
    }

    inst.activeSpeed = config.playbackSpeed;
    if (std::abs(config.trackTime - inst.lastPublishedTime) > 1.0e-5f) {
        inst.activeTime        = config.trackTime;
        inst.lastPublishedTime = config.trackTime;
        inst.dirtyPose         = true;
    }
    if (inst.finished && !config.finished) {
        inst.finished  = false;
        inst.dirtyPose = true;
    }
}

void AnimationContext::BindSkeletons(AnimationHandle handle, std::span<const SkeletonBinding> bindings) noexcept {
    const auto denseOpt = _impl->ResolveDense(handle);
    if (!denseOpt) {
        return;
    }
    InstanceState& inst   = _impl->instances[*denseOpt];
    const size_t   copied = std::min(bindings.size(), kMaxBoundSkeletons);
    std::copy_n(bindings.data(), copied, inst.skeletons.data());
    inst.skeletonCount = static_cast<uint32_t>(copied);
}

void AnimationContext::SetIKChains(AnimationHandle handle, std::span<const IKChain> chains) noexcept {
    const auto denseOpt = _impl->ResolveDense(handle);
    if (!denseOpt) {
        return;
    }
    InstanceState& inst   = _impl->instances[*denseOpt];
    const size_t   copied = std::min(chains.size(), kMaxIKChains);
    std::copy_n(chains.data(), copied, inst.ikChains.data());
    inst.ikChainCount = static_cast<uint32_t>(copied);
}

void AnimationContext::SuppressPaletteUpload(AnimationHandle handle) noexcept {
    if (const auto denseOpt = _impl->ResolveDense(handle)) {
        _impl->instances[*denseOpt].suppressPaletteUpload = true;
    }
}

void AnimationContext::Step(float dt, const AssetManager& assets, PoseUploadQueue& poseUploads) {
    const size_t activeCount = _impl->instances.size();
    if (activeCount == 0) {
        return;
    }

    _impl->stepPrefabs.assign(activeCount, nullptr);
    uint32_t totalPaletteJoints   = 0;
    uint32_t totalKinematicJoints = 0;

    for (size_t i = 0; i < activeCount; ++i) {
        InstanceState& inst           = _impl->instances[i];
        inst.uploadSliceCount         = 0;
        const auto prefabOpt          = assets.GetCachedPrefab(inst.prefabAsset);
        if (!prefabOpt) {
            continue;
        }
        const ModelPrefab& prefab     = *prefabOpt;
        _impl->stepPrefabs[i]         = &prefab;
        const auto requiredNodes      = static_cast<uint32_t>(prefab.nodes.size());

        if (inst.nodeCount != requiredNodes) {
            _impl->poses.Free(inst.poseOffset, inst.nodeCount);
            inst.poseOffset = _impl->poses.Allocate(requiredNodes);
            inst.nodeCount  = requiredNodes;
            inst.dirtyPose  = true;
        }

        if (!inst.suppressPaletteUpload) {
            for (uint32_t s = 0; s < inst.skeletonCount; ++s) {
                const SkeletonBinding& binding = inst.skeletons[s];
                if (binding.skeletonIndex >= prefab.skeletons.size()) {
                    continue;
                }
                const auto jointCount = static_cast<uint32_t>(prefab.skeletons[binding.skeletonIndex].joints.size());
                if (jointCount == 0) {
                    continue;
                }
                UploadSlice slice {
                    .jointOffset     = binding.jointOffset,
                    .paletteOffset   = totalPaletteJoints,
                    .kinematicOffset = totalKinematicJoints,
                    .jointCount      = jointCount,
                    .skeletonIndex   = binding.skeletonIndex,
                    .hasRagdoll      = binding.hasRagdoll,
                };
                totalPaletteJoints += jointCount;
                if (binding.hasRagdoll) {
                    totalKinematicJoints += jointCount;
                }
                inst.uploadSlices[inst.uploadSliceCount++] = slice;
            }
        }
    }

    _impl->paletteScratch.resize(totalPaletteJoints, JPH::Mat44::sIdentity());
    _impl->kinematicScratch.resize(totalKinematicJoints, JPH::Mat44::sIdentity());

    TaskSystem::ParallelFor(static_cast<uint32_t>(activeCount), 1, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            InstanceState&     inst   = _impl->instances[i];
            const ModelPrefab* prefab = _impl->stepPrefabs[i];
            if (prefab == nullptr) {
                continue;
            }

            const uint32_t nodeCount = inst.nodeCount;
            auto           clipLocal = _impl->poses.ClipLocal(inst.poseOffset, nodeCount);
            auto           local     = _impl->poses.Local(inst.poseOffset, nodeCount);
            auto           model     = _impl->poses.Model(inst.poseOffset, nodeCount);
            auto           morphs    = _impl->poses.Morphs(inst.poseOffset, nodeCount);
            auto           rigids    = _impl->poses.RigidNodes(inst.poseOffset, nodeCount);
            auto           scratch   = _impl->poses.Scratch(inst.poseOffset, nodeCount);

            const bool crossfading = (inst.prevTrack >= 0 && inst.blendFactor < 1.0f);
            const bool restingClip = inst.finished && !inst.dirtyPose && !crossfading;
            const bool hasIK       = inst.ikChainCount > 0;

            if (!restingClip && nodeCount > 0) {
                for (uint32_t n = 0; n < nodeCount; ++n) {
                    const Math::TransformTRS bindTRS = Math::Decompose(prefab->nodes[n].localTransform);
                    scratch[n].prev                  = SampledTRS {.translation = bindTRS.translation, .rotation = bindTRS.rotation, .scale = bindTRS.scale};
                    scratch[n].curr                  = scratch[n].prev;
                    morphs[n]                        = MorphSample {};
                }

                if (inst.blendDuration > 0.0f && inst.prevTrack >= 0) {
                    inst.blendFactor = std::min(1.0f, inst.blendFactor + (dt / inst.blendDuration));
                } else {
                    inst.blendFactor = 1.0f;
                }

                if (inst.prevTrack >= 0 && inst.blendFactor < 1.0f && static_cast<size_t>(inst.prevTrack) < prefab->animations.size()) {
                    const AnimationClip& prevClip = prefab->animations[static_cast<size_t>(inst.prevTrack)];
                    inst.prevTime += dt * inst.prevSpeed;
                    if (inst.prevTime >= prevClip.duration) {
                        inst.prevTime = std::fmod(inst.prevTime, std::max(prevClip.duration, 0.001f));
                    }
                    for (const AnimationChannel& channel: prevClip.channels) {
                        if (channel.targetNodeIndex < 0 || static_cast<uint32_t>(channel.targetNodeIndex) >= nodeCount) {
                            continue;
                        }
                        if (channel.path != AnimationPathType::Weights) {
                            SampleTRSChannel(channel, inst.prevTime, scratch[static_cast<size_t>(channel.targetNodeIndex)].prev);
                        }
                    }
                }

                if (inst.activeTrack >= 0 && static_cast<size_t>(inst.activeTrack) < prefab->animations.size()) {
                    const AnimationClip& clip = prefab->animations[static_cast<size_t>(inst.activeTrack)];
                    inst.activeTime += dt * inst.activeSpeed;
                    if (inst.activeTime >= clip.duration) {
                        inst.activeTime = inst.loop ? std::fmod(inst.activeTime, std::max(clip.duration, 0.001f)) : clip.duration;
                        if (!inst.loop) {
                            inst.finished = true;
                        }
                    }

                    for (const AnimationChannel& channel: clip.channels) {
                        if (channel.targetNodeIndex < 0 || static_cast<uint32_t>(channel.targetNodeIndex) >= nodeCount) {
                            continue;
                        }
                        const auto node = static_cast<size_t>(channel.targetNodeIndex);
                        if (channel.path == AnimationPathType::Weights) {
                            morphs[node] = SampleMorphChannel(channel, inst.activeTime);
                        } else {
                            SampleTRSChannel(channel, inst.activeTime, scratch[node].curr);
                        }
                    }
                }

                for (uint32_t n = 0; n < nodeCount; ++n) {
                    const NodeEvalScratch& state = scratch[n];
                    if (inst.prevTrack >= 0 && inst.blendFactor < 1.0f) {
                        const float     t        = inst.blendFactor;
                        const JPH::Vec3 blendedT = state.prev.translation + t * (state.curr.translation - state.prev.translation);
                        const JPH::Quat blendedR = state.prev.rotation.SLERP(state.curr.rotation, t).Normalized();
                        const JPH::Vec3 blendedS = state.prev.scale + t * (state.curr.scale - state.prev.scale);
                        clipLocal[n]             = JPH::Mat44::sRotationTranslation(blendedR, blendedT).PreScaled(blendedS);
                    } else {
                        clipLocal[n] = JPH::Mat44::sRotationTranslation(state.curr.rotation, state.curr.translation).PreScaled(state.curr.scale);
                    }
                }

                if (inst.blendFactor >= 1.0f) {
                    inst.prevTrack = -1;
                }
            } else if (!restingClip && nodeCount == 0) {
                if (inst.activeTrack >= 0 && static_cast<size_t>(inst.activeTrack) < prefab->animations.size()) {
                    const AnimationClip& clip = prefab->animations[static_cast<size_t>(inst.activeTrack)];
                    inst.activeTime += dt * inst.activeSpeed;
                    if (inst.activeTime >= clip.duration) {
                        inst.activeTime = inst.loop ? std::fmod(inst.activeTime, std::max(clip.duration, 0.001f)) : clip.duration;
                        if (!inst.loop) {
                            inst.finished = true;
                        }
                    }
                }
            }

            const bool needPoseSolve = (!restingClip || hasIK || inst.hadIKLastStep) && nodeCount > 0;
            if (needPoseSolve) {
                std::ranges::copy(clipLocal, local.begin());
                ResolveBaseFK(*prefab, local, model, scratch);

                if (hasIK) {
                    ApplyIKAndPropagateDescendantFK(
                        *prefab, std::span<const IKChain> {inst.ikChains.data(), inst.ikChainCount}, local, model, scratch
                    );
                }

                for (uint32_t n = 0; n < nodeCount; ++n) {
                    if (prefab->nodes[n].hasMesh) {
                        rigids[n] = RigidNodeSlot {.trs = Math::Decompose(model[n]), .valid = true};
                    } else {
                        rigids[n] = RigidNodeSlot {};
                    }
                }
            }

            inst.hadIKLastStep     = hasIK;
            inst.dirtyPose         = false;
            inst.lastPublishedTime = inst.activeTime;

            for (uint32_t s = 0; s < inst.uploadSliceCount; ++s) {
                const UploadSlice& slice    = inst.uploadSlices[s];
                const Skeleton&    skeleton = prefab->skeletons[slice.skeletonIndex];
                for (uint32_t j = 0; j < slice.jointCount; ++j) {
                    const Joint&     joint      = skeleton.joints[j];
                    const JPH::Mat44 jointModel = (joint.nodeIndex >= 0 && static_cast<uint32_t>(joint.nodeIndex) < nodeCount) ?
                                                      model[static_cast<size_t>(joint.nodeIndex)] :
                                                      JPH::Mat44::sIdentity();
                    _impl->paletteScratch[slice.paletteOffset + j] = jointModel * joint.inverseBindMatrix;
                    if (slice.hasRagdoll) {
                        _impl->kinematicScratch[slice.kinematicOffset + j] = jointModel;
                    }
                }
            }
        }
    });

    for (size_t i = 0; i < activeCount; ++i) {
        InstanceState& inst = _impl->instances[i];
        for (uint32_t s = 0; s < inst.uploadSliceCount; ++s) {
            const UploadSlice& slice = inst.uploadSlices[s];
            poseUploads.Push(
                slice.jointOffset,
                std::span<const JPH::Mat44> {_impl->paletteScratch.data() + slice.paletteOffset, slice.jointCount}
            );
            if (slice.hasRagdoll) {
                poseUploads.PushKinematic(
                    slice.jointOffset,
                    std::span<const JPH::Mat44> {_impl->kinematicScratch.data() + slice.kinematicOffset, slice.jointCount}
                );
            }
        }
        inst.suppressPaletteUpload = false;
        inst.ikChainCount          = 0;
    }
}

auto AnimationContext::Playback(AnimationHandle handle) const noexcept -> ZHLN::Optional<PlaybackState> {
    return _impl->ResolveDense(handle).transform([this](uint32_t denseIdx) {
        const InstanceState& inst = _impl->instances[denseIdx];
        return PlaybackState {.trackTime = inst.activeTime, .finished = inst.finished};
    });
}

auto AnimationContext::Morph(AnimationHandle handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<MorphSample> {
    return _impl->ResolveDense(handle).and_then([this, nodeIndex](uint32_t denseIdx) -> ZHLN::Optional<MorphSample> {
        const InstanceState& inst = _impl->instances[denseIdx];
        if (nodeIndex >= inst.nodeCount) {
            return std::nullopt;
        }
        const MorphSample sample = _impl->poses.Morphs(inst.poseOffset, inst.nodeCount)[nodeIndex];
        if (sample.activeCount == 0) {
            return std::nullopt;
        }
        return sample;
    });
}

auto AnimationContext::RigidNode(AnimationHandle handle, uint32_t nodeIndex) const noexcept -> ZHLN::Optional<Math::TransformTRS> {
    return _impl->ResolveDense(handle).and_then([this, nodeIndex](uint32_t denseIdx) -> ZHLN::Optional<Math::TransformTRS> {
        const InstanceState& inst = _impl->instances[denseIdx];
        if (nodeIndex >= inst.nodeCount) {
            return std::nullopt;
        }
        const RigidNodeSlot& slot = _impl->poses.RigidNodes(inst.poseOffset, inst.nodeCount)[nodeIndex];
        if (!slot.valid) {
            return std::nullopt;
        }
        return slot.trs;
    });
}

auto AnimationContext::LocalPose(AnimationHandle handle) const noexcept -> std::span<const JPH::Mat44> {
    const auto denseOpt = _impl->ResolveDense(handle);
    if (!denseOpt) {
        return {};
    }
    const InstanceState& inst = _impl->instances[*denseOpt];
    return _impl->poses.Local(inst.poseOffset, inst.nodeCount);
}

auto AnimationContext::ModelPose(AnimationHandle handle) const noexcept -> std::span<const JPH::Mat44> {
    const auto denseOpt = _impl->ResolveDense(handle);
    if (!denseOpt) {
        return {};
    }
    const InstanceState& inst = _impl->instances[*denseOpt];
    return _impl->poses.Model(inst.poseOffset, inst.nodeCount);
}

} // namespace ZHLN::Animation
