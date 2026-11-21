// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AnimationID.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <type_traits>
namespace ZHLN::Animation {
struct AnimatorComponent {
    AnimationID animationID          = AnimationID::Null();
    AssetID     prefabAsset          = InvalidAssetID;
    int32_t     currentTrackIdx      = -1;
    float       currentTrackTime     = 0.0f;
    float       currentPlaybackSpeed = 1.0f;
    float       blendDuration        = 0.15f;
    bool        currentLoop          = true;
    bool        isFinished           = false;
};
static_assert(std::is_trivially_copyable_v<AnimatorComponent>);

// COM-safe playback controls. AnimatorComponent is read-only runtime state.
// Attach to an animated prefab root. Seek is a consumed one-shot request.
struct AnimatorControlComponent {
    int32_t trackIndex    = -1;
    float   playbackSpeed = 1.0f;
    float   blendDuration = 0.15f;
    float   seekTime      = 0.0f;
    bool    loop          = true;
    bool    seekRequested = false;
};

static_assert(std::is_trivially_copyable_v<AnimatorControlComponent>);
} // namespace ZHLN::Animation

