// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Engine-only graph entry point; the audio library exposes AudioContext, not
// engine component queries. Keep Components.hpp out of its public API.
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {

ZHLN_API void AudioSystem(
    ECS::Query<const Components::AudioListenerComponent, const Components::WorldTransformComponent,
               const Components::TransformComponent, Components::AudioSourceComponent&, Components::LoopSynthComponent&> query,
    ZHLN::Optional<AudioContext&> audio, ECS::Query<const Components::CameraComponent> cameraQuery, FrameDt dt
);

} // namespace ZHLN
