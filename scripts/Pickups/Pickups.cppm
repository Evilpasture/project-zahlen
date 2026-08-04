// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <format>
#include <string>
#include <vector>

export module ZHLN.Pickups;

import ZHLN.Weapons;
import ZHLN.CombatAudio;

export namespace ZHLN::Pickups {

struct WeaponPickup {
    Entity            entity   = NullEntity;
    Weapons::WeaponId weaponId = Weapons::WeaponId::Rifle;
    int32_t           ammo     = 30;
    JPH::Vec3         position = JPH::Vec3::sZero();
    JPH::Vec3         velocity = JPH::Vec3::sZero();
    JPH::Vec3         spin     = JPH::Vec3::sZero();
    float             age      = 0.0f;
    float             ttl      = 45.0f;
    bool              rested   = false;
    bool              claimed  = false;
};

template <typename PlayerController>
void UpdatePickupsSystem(Engine* engine, PlayerController& p, std::vector<WeaponPickup>& pickups, Entity playerEnt, float dt) {
    auto& reg = engine->GetRegistry();

    JPH::Vec3 playerPos = JPH::Vec3::sZero();
    if (playerEnt != NullEntity && reg.IsAlive(playerEnt)) {
        trans = playerEnt;Components::TransformComponent>(reg, playerEnt, [&](auto& trans) {
            playerPos = trans->position;
});    }

    if (p.pickupFlash > 0.0f) {
        p.pickupFlash = std::max(0.0f, p.pickupFlash - dt);
    }

    for (auto it = pickups.begin(); it != pickups.end();) {
        it->age += dt;

        if (!it->rested) {
            it->velocity.SetY(it->velocity.GetY() - 20.0f * dt);
            it->position += it->velocity * dt;

            JPH::Quat rotX = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), it->spin.GetX() * dt);
            JPH::Quat rotY = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), it->spin.GetY() * dt);
            JPH::Quat rotZ = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), it->spin.GetZ() * dt);
            JPH::Quat rot  = rotX * rotY * rotZ;

            if (it->entity != NullEntity && reg.IsAlive(it->entity)) {
                Patch<Components::TransformComponent>(reg, it->entity, [&](auto& trans) {
                    trans.position = it->position;
                    trans.rotation = (rot * trans.rotation).Normalized();
});            }

            if (it->position.GetY() < 0.05f) {
                it->position.SetY(0.05f);
                it->velocity *= 0.35f;
                it->velocity.SetY(it->velocity.GetY() * -0.3f);
                it->spin *= 0.3f;
                if (it->velocity.LengthSq() < 0.15f) {
                    it->rested = true;
                }
            }
        }

        float distSq = (it->position - playerPos).LengthSq();
        if (p.alive && distSq < 1.5f * 1.5f && !it->claimed) {
            auto&   ammoState = p.ammo[static_cast<size_t>(it->weaponId)];
            int32_t cap       = Weapons::GetWeaponDef(it->weaponId).startReserve * 2;
            int32_t room      = std::max(0, cap - ammoState.reserve);

            if (room > 0) {
                int32_t take = std::min(it->ammo, room);
                ammoState.reserve += take;
                it->ammo -= take;
                it->claimed = (it->ammo <= 0);

                CombatAudio::PlayPickup(engine);
                p.pickupFlash     = 1.4f;
                p.pickupFlashText = std::format("+{} {}", take, Weapons::GetWeaponDef(it->weaponId).caliber);
            }
        }

        if (it->claimed || it->age >= it->ttl) {
            if (it->entity != NullEntity && reg.IsAlive(it->entity)) {
                reg.Destroy(it->entity);
            }
            it = pickups.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace ZHLN::Pickups
