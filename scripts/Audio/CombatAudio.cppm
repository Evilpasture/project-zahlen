// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Zahlen/Audio.hpp>
#include <Zahlen/Engine.hpp>
#include <algorithm>

export module ZHLN.CombatAudio;

export namespace ZHLN::CombatAudio {

inline void PlayShoot(Engine* engine, float dist) {
    float atten = std::max(0.08f, 1.0f - dist / 55.0f);
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(180.0f, 0.16f, 0.35f * atten);
    audio.PlayProceduralBeep(1400.0f, 0.14f, 0.50f * atten);
}

inline void PlayShotgun(Engine* engine, float dist) {
    float atten = std::max(0.08f, 1.0f - dist / 65.0f);
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(190.0f, 0.30f, 0.60f * atten);
    audio.PlayProceduralBeep(900.0f, 0.25f, 0.75f * atten);
}

inline void PlayPump(Engine* engine, float dist) {
    if (dist > 30.0f)
        return;
    float atten = std::max(0.1f, 1.0f - dist / 28.0f);
    engine->GetAudioContext().PlayProceduralBeep(2400.0f, 0.05f, 0.30f * atten);
}

inline void PlayMinigun(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(1900.0f, 0.05f, 0.25f);
    audio.PlayProceduralBeep(150.0f, 0.06f, 0.30f);
}

inline void PlayImpact(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(2600.0f, 0.07f, 0.25f * atten);
}

inline void PlayFlesh(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(420.0f, 0.10f, 0.40f * atten);
}

inline void PlayHitmark(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(1500.0f, 0.05f, 0.14f);
}

inline void PlayKill(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(700.0f, 0.18f, 0.18f);
}

inline void PlayHurt(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(120.0f, 0.30f, 0.30f);
    audio.PlayProceduralBeep(260.0f, 0.25f, 0.50f);
}

inline void PlayStep(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(900.0f, 0.06f, 0.09f);
}

inline void PlayEmpty(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(4200.0f, 0.03f, 0.20f);
}

inline void PlayReload(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(3000.0f, 0.06f, 0.30f);
    audio.PlayProceduralBeep(1800.0f, 0.08f, 0.30f);
    audio.PlayProceduralBeep(2600.0f, 0.06f, 0.35f);
}

inline void PlayBlast(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(150.0f, 0.55f, 0.60f);
    audio.PlayProceduralBeep(320.0f, 0.40f, 0.35f);
}

inline void PlayPickup(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(1400.0f, 0.09f, 0.22f);
    audio.PlayProceduralBeep(2000.0f, 0.07f, 0.16f);
}

inline void PlayPierce(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(3400.0f, 0.05f, 0.18f);
}

} // namespace ZHLN::CombatAudio
