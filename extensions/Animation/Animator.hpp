// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Animation/IKControls.hpp>
#include <Zahlen/ECS/Entity.hpp>
namespace ZHLN::COM {
struct IEntities;
}
namespace ZHLN::Animator {
// Host-side convenience over published POD controls, not a native service or
// a callback into the engine. Call at an isolated host phase boundary.
struct PlaybackSnapshot {
    int32_t trackIndex = -1;
    float   time       = 0;
    float   speed      = 1;
    bool    loop       = true;
    bool    finished   = false;
};
// Requires a live animated prefab root; never creates/copies a runtime handle.
bool Attach(COM::IEntities&, Entity);
bool Read(COM::IEntities&, Entity, PlaybackSnapshot&);
bool Play(COM::IEntities&, Entity, int32_t track, float blendDuration = 0.15f, bool loop = true);
bool Seek(COM::IEntities&, Entity, float seconds);
bool Speed(COM::IEntities&, Entity, float multiplier);
bool SetIK(COM::IEntities&, Entity, const IK::TwoBoneIKComponent&);
// Removes host controls/IK, not the Animator or prefab resources. Playback
// continues with its last state unless Speed(0)/Play(-1) was requested first.
void Detach(COM::IEntities&, Entity);
} // namespace ZHLN::Animator
