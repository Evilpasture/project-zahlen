// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPInteraction.hpp
//
// What the map says about its interactive entities, decoded.
//
// This is the asset half of what used to be one file in
// gameplay/Interaction/BSPInteraction.cpp. Everything Valve-shaped lives here:
// the class names (func_door, trigger_teleport), the spawnflag bits, the
// key names, the movedir sentinels, the 0x1B output separators, the degree
// conventions. What is left over is a record per entity, in engine terms, and
// that is all a consumer sees.
//
// The split is not a style preference, it is the layer rule the tree already
// enforces (configure/check_layer_dag.py): a plugin may not include a gameplay
// header, so the file that produces DoorComponent/ButtonComponent cannot live
// here. What can live here is the decoding -- and the decode is the part that
// had no business being in the layer that decides what a door *means*.
//
// The name is shared with gameplay/Interaction/BSPInteraction.hpp on purpose --
// same file, split across the layer boundary -- and the two never collide:
// a quoted #include "BSPInteraction.hpp" resolves beside its includer (so each
// layer's own file), and <Interaction/BSPInteraction.hpp> reaches the gameplay
// one from anywhere.
//
// Nothing here writes to the registry. Describe() reads it to resolve which
// spawned entity a lump entry refers to and returns descriptions; attaching
// components is the caller's job. A second consumer (a tool, a validator, a
// test) can therefore decode a map without spawning anything.

#include "BSPGeometry.hpp"
#include "BSPRead.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::BSP {

// --- Entity output decoding -------------------------------------------------
//
// One entity output, decoded. Source's wire form is
//
//     <target>,<input>,<parameter>,<delay>,<timesToFire>
//
// and GMod-branch VBSP writes the separators as 0x1B instead of ',' --
// "Door3\x1bToggle\x1b\x1b0\x1b-1". Splitting on comma alone turned every such
// output into one unmatchable target string and silently severed
// button/trigger -> door wiring, which is why decoding happens here, once,
// rather than at each call site.
struct EntityOutput {
    std::string key;            // the output's name, e.g. "OnTrigger"
    std::string target;         // the targetname it fires at
    std::string input;          // the input it fires; "Toggle" when the field is absent
    std::string parameter;      // override parameter, empty when absent
    float       delay       = 0.0f;
    int32_t     timesToFire = -1; // -1 means indefinitely
};

// Every key on @p entity that parses as an output, in lump order. A key is an
// output when its name starts with "On" and its value is not empty.
[[nodiscard]] auto ParseEntityOutputs(const BSPEntity& entity) -> std::vector<EntityOutput>;

// The entity's "movedir" as an engine-space direction. Source spells it as
// pitch/yaw degrees and overloads the first component with two sentinels
// instead of an angle: -1 is up, -2 is down. An absent key is +X forward.
[[nodiscard]] auto ParseMoveDirection(const BSPEntity& entity, const ImportOptions& options) -> JPH::Vec3;

// The numeric reading of a keyvalue or of an output's parameter field.
// Absent or unparseable yields @p fallback, the field's own default: reading a
// missing key as 0.0 would turn "speed not specified" into "a door that does
// not move".
[[nodiscard]] auto ParseFloatOr(std::string_view text, float fallback) noexcept -> float;

// --- The decoded record -----------------------------------------------------

// What an entity's class name means, once. The set is the one this plugin
// knows how to describe; anything else is not interactive and is skipped.
enum class InteractiveKind : uint8_t {
    None,
    TeleportDestination, // info_teleport_destination
    Door,                // func_door, momentary_door
    DoorRotating,        // func_door_rotating, prop_door_rotating
    Button,              // func_button, momentary_rot_button
    ButtonRotating,      // func_rot_button
    Trigger,             // trigger_* except the two below
    TriggerOnce,         // trigger_once
    TriggerTeleport,     // trigger_teleport
};

// Spawnflag bits, decoded. `spawnflags` is a bit field whose meanings are
// Valve's, per class, and a consumer that has to test bit 512 to learn that a
// button responds to touch is a consumer doing format work.
struct DoorFlags {
    bool startsOpen    = false; // bit 1
    bool rotateBack    = false; // bit 2: reverse the rotation direction
    bool toggle        = false; // bit 32
    bool axisX         = false; // bit 64
    bool axisY         = false; // bit 128
    bool useOpens      = false; // bits 256 and 512, both spellings
    bool touchOpens    = false; // bit 1024
    bool locked        = false; // bit 2048
};

struct ButtonFlags {
    bool dontMove        = false; // bit 1
    bool toggle          = false; // bit 32
    bool touchActivates  = false; // bit 512
    bool damageActivates = false; // bit 1024
    bool useActivates    = false; // bit 2048
};

// One interactive lump entity, decoded and resolved.
//
// `entity` is the spawned ECS entity the lump entry refers to, found by
// targetname or by the model-source fallback; Entity::Null() means the lump
// entry exists but nothing in the scene answers to it, and the caller skips it.
// The scalar fields are optional because "the map did not set it" and "the map
// set it to zero" are different answers, and only the consumer knows the
// default (a door's 100 unit/s is a game rule, not a file format).
struct EntityRecord {
    Entity          entity = Entity::Null();
    InteractiveKind kind   = InteractiveKind::None;

    std::string className;
    std::string targetName;
    std::string model;
    std::string target;
    std::string message;
    std::string filterName;
    uint32_t    spawnFlags = 0;

    std::optional<float> speed;
    std::optional<float> wait;
    std::optional<float> lip;
    std::optional<float> distance; // angular travel, for rotating doors
    std::optional<float> damage;

    // Engine space, sentinels resolved, and +X forward when the lump has no
    // movedir key -- the absent case is a format default, so it is decided here
    // rather than by every consumer.
    JPH::Vec3 moveDirection = JPH::Vec3::sAxisX();

    // Only for TeleportDestination, and already in engine space: the landing
    // point and the facing. The degree convention and the axis remap are the
    // asset's, so they are resolved before the record exists.
    std::optional<JPH::Vec3> origin;
    JPH::Quat               orientation = JPH::Quat::sIdentity();

    std::optional<DoorFlags>   door;
    std::optional<ButtonFlags> button;

    std::vector<EntityOutput> outputs;

    // The `<targetname>` fallback used when an entity has no targetname: the
    // deterministic "<classname>_<modelIndex>" name the scene writer generates,
    // or the class name itself for a non-brush entity.
    std::string matchName;
};

// Every interactive entity in @p map, decoded, with `entity` resolved against
// the entities already in @p registry. The registry is read, never written:
// this describes what should happen, and the caller does it.
[[nodiscard]] auto DescribeInteractiveEntities(ECS::Registry& registry, const BSPMap& map, const ImportOptions& options = {}) -> std::vector<EntityRecord>;

// The entity the lump entry @p matchName / @p modelSource refers to, or
// Entity::Null(). A model source ("*12") is matched against the scene's brush
// submodels first, because a targetname is explicitly not an identity: Source
// lets many brush entities share one so an input can address the whole group,
// and matching by name alone attaches every member of a named door bank to the
// first entity that answers.
[[nodiscard]] auto FindMatchingEntity(ECS::Registry& registry, std::string_view matchName, std::string_view modelSource) -> Entity;

} // namespace ZHLN::BSP
