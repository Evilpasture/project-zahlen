// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Engine-only graph entry point; the audio library exposes AudioContext, not
// engine component queries. Keep Components.hpp out of its public API.
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {

ZHLN_API void AudioSystem(
    ECS::Query<
        const Components::AudioListenerComponent,
        const Components::WorldTransformComponent,
        const Components::TransformComponent,
        Components::AudioSourceComponent&
    >                                             query,
    ZHLN::Optional<AudioContext&>                 audio,
    ZHLN::Optional<AssetManager&>                 assets,
    ECS::Query<const Components::CameraComponent> cameraQuery,
    FrameDt                                       dt
);

} // namespace ZHLN
