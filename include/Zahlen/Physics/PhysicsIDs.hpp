// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

// Naming taxonomy for the four concepts that used to share the word "Handle":
//
//   1. inert numeric key         -> *ID   (BodyID, RagdollID)
//   2. non-owning domain facade  -> a noun (Simulation, GraphicsDevice)
//   3. scoped begin/end token    -> *Scope (FrameScope, SessionScope)
//   4. owning allocation         -> std::unique_ptr<T> / *Instance
//
// A physics slot is a key, not a body: it owns nothing and has no methods that
// act on the simulation. Hand it to the PhysicsContext/Simulation capability.
//
// Physics slots are not ECS entities. The 64-bit packing is shared with Jolt
// user data, but the distinct type prevents accidental cross-service use.
struct BodyID {
    uint32_t index;
    uint32_t generation;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
    [[nodiscard]] static constexpr auto Unpack(uint64_t raw) noexcept -> BodyID {
        return {.index = static_cast<uint32_t>(raw), .generation = static_cast<uint32_t>(raw >> 32)};
    }
    [[nodiscard]] static constexpr auto Null() noexcept -> BodyID {
        return {.index = UINT32_MAX, .generation = UINT32_MAX};
    }
    constexpr auto operator==(const BodyID&) const noexcept -> bool = default;
};

// A ragdoll uses a separate generational pool from rigid bodies.
enum class RagdollID : uint64_t { Invalid = UINT64_MAX };

static_assert(sizeof(BodyID) == 8 && std::is_trivially_default_constructible_v<BodyID> && std::is_trivially_copyable_v<BodyID>);
static_assert(sizeof(RagdollID) == 8 && std::is_trivially_copyable_v<RagdollID>);

}
