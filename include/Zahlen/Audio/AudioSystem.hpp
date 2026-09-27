// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The graph entry point depends on component and generated GPU layouts. Keep
// that dependency out of Audio.hpp, which is also used by the audio-only target.
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {

ZHLN_API void AudioSystem(
    ECS::Query<const Components::AudioListenerComponent, const Components::WorldTransformComponent,
               const Components::TransformComponent, Components::AudioSourceComponent&, Components::LoopSynthComponent&> query,
    ECS::OptionRes<AudioContext> audio, ECS::OptionRes<Camera> camera, FrameDt dt
);

} // namespace ZHLN
