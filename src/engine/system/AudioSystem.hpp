// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Engine-only graph entry point; the audio library exposes AudioContext, not
// engine component queries. Keep Components.hpp out of its public API.
#include <engine/AssetManager.hpp>
#include <audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>

namespace ZHLN {

void AudioSystem(
    ECS::Query<
        const Components::AudioListenerComponent,
        const Components::WorldTransformComponent,
        const Components::TransformComponent,
        Components::AudioSourceComponent&
    >                                             query,
    ZHLN::Optional<AudioContext&>                 audio,
    ECS::Res<AssetManager>                        assets,
    ECS::Query<const Components::CameraComponent> cameraQuery,
    FrameDt                                       dt
);

} // namespace ZHLN
