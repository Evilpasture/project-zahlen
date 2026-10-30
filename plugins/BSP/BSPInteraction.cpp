// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPInteraction.cpp
//
// The interactive half of the entity lump, decoded. The registry is read to
// resolve names and never written: this file answers "what does the map say
// about this door", and the caller answers "what does a door do".

#include "BSPInteraction.hpp"

#include "BSPScene.hpp" // ConvertDirection / ConvertPosition / EngineEulerAnglesFromSourceAngles
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace ZHLN::BSP {
namespace {

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

// Source's sentinels for the first movedir component: not angles, directions.
constexpr float kMoveDirSentinelUp   = -1.0f;
constexpr float kMoveDirSentinelDown = -2.0f;
constexpr float kSentinelEpsilon     = 0.01f;

[[nodiscard]] auto IsSentinel(const float v[3], float sentinel) noexcept -> bool {
    return std::abs(v[0] - sentinel) < kSentinelEpsilon && std::abs(v[1]) < kSentinelEpsilon && std::abs(v[2]) < kSentinelEpsilon;
}

// The three components of a space separated keyvalue, or nothing when the key
// is absent or unparseable.
[[nodiscard]] auto OptionalVec3(const BSPEntity& entity, std::string_view key) -> std::optional<JPH::Vec3> {
    const auto arr = entity.FindVector(key);
    if (!arr) {
        return std::nullopt;
    }
    return JPH::Vec3((*arr)[0], (*arr)[1], (*arr)[2]);
}

} // namespace

// --- Decoders ---------------------------------------------------------------

auto ParseFloatOr(std::string_view text, float fallback) noexcept -> float {
    if (text.empty()) {
        return fallback;
    }
    const char* cur = text.data();
    const char* end = text.data() + text.size();
    while (cur < end && (*cur == ' ' || *cur == '\t')) {
        cur++;
    }
    if (cur >= end) {
        return fallback;
    }
    float val = fallback;
    if (auto [ptr, ec] = std::from_chars(cur, end, val); ec == std::errc {}) {
        return val;
    }
    return fallback;
}

auto ParseEntityOutputs(const BSPEntity& entity) -> std::vector<EntityOutput> {
    std::vector<EntityOutput> outputs;

    for (const auto& [key, rawValue]: entity.keys) {
        if (!key.starts_with("On") || rawValue.empty()) {
            continue;
        }

        // The separator substitution is the whole reason this lives here. It has
        // to happen before the split, on a copy: the lump's bytes are not ours
        // to rewrite.
        std::string      normalised;
        std::string_view value = rawValue;
        if (value.find('\x1b') != std::string_view::npos) {
            normalised.assign(rawValue);
            for (char& c: normalised) {
                if (c == '\x1b') {
                    c = ',';
                }
            }
            value = normalised;
        }

        EntityOutput out;
        out.key = key;

        // Fields are optional to the right -- the writer emits as many as the
        // mapper filled in, and a missing input means "toggle". The chain of
        // find() calls is the format, spelled out once.
        const size_t c1 = value.find(',');
        if (c1 == std::string::npos) {
            out.target = value;
            out.input  = "Toggle";
            outputs.push_back(std::move(out));
            continue;
        }

        out.target      = value.substr(0, c1);
        const size_t c2 = value.find(',', c1 + 1);
        if (c2 == std::string::npos) {
            out.input = value.substr(c1 + 1);
            outputs.push_back(std::move(out));
            continue;
        }

        out.input       = value.substr(c1 + 1, c2 - c1 - 1);
        const size_t c3 = value.find(',', c2 + 1);
        if (c3 == std::string::npos) {
            out.parameter = value.substr(c2 + 1);
            outputs.push_back(std::move(out));
            continue;
        }

        out.parameter   = value.substr(c2 + 1, c3 - c2 - 1);
        const size_t c4 = value.find(',', c3 + 1);
        if (c4 == std::string::npos) {
            out.delay = ParseFloatOr(value.substr(c3 + 1), 0.0f);
            outputs.push_back(std::move(out));
            continue;
        }

        out.delay = ParseFloatOr(value.substr(c3 + 1, c4 - c3 - 1), 0.0f);
        // An empty or unparseable count is "fire forever", which is what the
        // format spells -1; atoi's 0 would disarm the output instead.
        const std::string_view countText = value.substr(c4 + 1);
        out.timesToFire                  = static_cast<int32_t>(std::atoi(std::string(countText).c_str()));
        if (countText.find_first_not_of(" \t") == std::string_view::npos) {
            out.timesToFire = -1;
        }
        outputs.push_back(std::move(out));
    }

    return outputs;
}

auto ParseMoveDirection(const BSPEntity& entity, const ImportOptions& options) -> JPH::Vec3 {
    const std::string_view movedirStr = entity.Find("movedir");
    if (movedirStr.empty()) {
        return ConvertDirection(options, JPH::Vec3::sAxisX());
    }

    float       v[3] = {0.0f, 0.0f, 0.0f};
    const char* cur  = movedirStr.data();
    const char* end  = movedirStr.data() + movedirStr.size();
    for (float& component: v) {
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            cur++;
        }
        if (cur >= end) {
            break;
        }
        auto [ptr, ec] = std::from_chars(cur, end, component);
        if (ec != std::errc {}) {
            break;
        }
        cur = ptr;
    }

    // The sentinels are checked before the angle reading: reading -1 as a pitch
    // would aim the mover at 359 degrees instead of straight up.
    if (IsSentinel(v, kMoveDirSentinelUp)) {
        return ConvertDirection(options, JPH::Vec3(0.0f, 0.0f, 1.0f));
    }
    if (IsSentinel(v, kMoveDirSentinelDown)) {
        return ConvertDirection(options, JPH::Vec3(0.0f, 0.0f, -1.0f));
    }

    const float     pitch = v[0] * kDegreesToRadians;
    const float     yaw   = v[1] * kDegreesToRadians;
    const float     cp    = std::cos(pitch);
    const JPH::Vec3 sourceDir(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
    if (sourceDir.LengthSq() < 1e-4f) {
        return ConvertDirection(options, JPH::Vec3(1.0f, 0.0f, 0.0f));
    }
    return ConvertDirection(options, sourceDir.Normalized());
}

// --- Resolution -------------------------------------------------------------

auto FindMatchingEntity(ECS::Registry& registry, std::string_view matchName, std::string_view modelSource) -> Entity {
    if (!modelSource.empty()) {
        // A targetname is not an entity identity: Source deliberately allows
        // many brush entities to share one so an input can address the whole
        // group. Match the unique brush submodel first; otherwise every member
        // of a named door bank attaches its components to the first entity.
        std::string modelSuffix;
        if (modelSource.starts_with('*')) {
            modelSuffix = "#model_" + std::string(modelSource.substr(1));
        }
        for (const Entity e: registry.GetEntitiesWith<Components::SceneSourceComponent>()) {
            const auto srcComp = registry.Get<Components::SceneSourceComponent>(e);
            if (!srcComp) {
                continue;
            }
            const std::string_view source(srcComp->source);
            if (source == modelSource || (!modelSuffix.empty() && source.ends_with(modelSuffix))) {
                return e;
            }
        }
    }
    for (const Entity e: registry.GetEntitiesWith<Components::NameComponent>()) {
        const auto nameComp = registry.Get<Components::NameComponent>(e);
        if (nameComp && std::string_view(nameComp->name) == matchName) {
            return e;
        }
    }
    return Entity::Null();
}

// --- The describe pass ------------------------------------------------------

namespace {

// The class -> kind table, and the only place a class name appears.
[[nodiscard]] auto ClassifyInteractive(std::string_view className) noexcept -> InteractiveKind {
    if (className == "info_teleport_destination") {
        return InteractiveKind::TeleportDestination;
    }
    if (className == "func_door" || className == "momentary_door") {
        return InteractiveKind::Door;
    }
    if (className == "func_door_rotating" || className == "prop_door_rotating") {
        return InteractiveKind::DoorRotating;
    }
    if (className == "func_button" || className == "momentary_rot_button") {
        return InteractiveKind::Button;
    }
    if (className == "func_rot_button") {
        return InteractiveKind::ButtonRotating;
    }
    if (className == "trigger_once") {
        return InteractiveKind::TriggerOnce;
    }
    if (className == "trigger_teleport") {
        return InteractiveKind::TriggerTeleport;
    }
    if (className.starts_with("trigger_")) {
        return InteractiveKind::Trigger;
    }
    return InteractiveKind::None;
}

// The spawnflag bits, per class. Named tests rather than a bit constant per
// call site: the numbers are Valve's, and they only appear here.
//
// The bits below are transcribed from what this importer has always read, not
// from a bit table: 256 and 512 are both "use opens" (Valve renumbered it
// between branches and maps carry both), and 1024 is "touch opens". Reading
// them per a canonical table would silently change which doors open on touch.
[[nodiscard]] auto DecodeDoorFlags(uint32_t spawnFlags) -> DoorFlags {
    DoorFlags flags;
    flags.startsOpen = (spawnFlags & 1) != 0;
    flags.rotateBack = (spawnFlags & 2) != 0;
    flags.toggle     = (spawnFlags & 32) != 0;
    flags.axisX      = (spawnFlags & 64) != 0;
    flags.axisY      = (spawnFlags & 128) != 0;
    flags.useOpens   = (spawnFlags & 256) != 0 || (spawnFlags & 512) != 0;
    flags.touchOpens = (spawnFlags & 1024) != 0;
    flags.locked     = (spawnFlags & 2048) != 0;
    return flags;
}

[[nodiscard]] auto DecodeButtonFlags(uint32_t spawnFlags) -> ButtonFlags {
    ButtonFlags flags;
    flags.dontMove        = (spawnFlags & 1) != 0;
    flags.toggle          = (spawnFlags & 32) != 0;
    flags.touchActivates  = (spawnFlags & 512) != 0;
    flags.damageActivates = (spawnFlags & 1024) != 0;
    flags.useActivates    = (spawnFlags & 2048) != 0;
    return flags;
}

// The name a scene writer gave an entity that has none of its own:
// "<classname>_<modelIndex>" for a brush, the class name otherwise. Keeping the
// two in step is what makes an unnamed entity addressable at all, which is why
// this is one function and not a copy of the rule at each caller.
[[nodiscard]] auto FallbackMatchName(std::string_view className, std::string_view modelSource) -> std::string {
    std::string matchName(className);
    if (modelSource.size() > 1 && modelSource[0] == '*') {
        int         modelIndex = -1;
        const char* beg        = modelSource.data() + 1;
        std::from_chars(beg, beg + (modelSource.size() - 1), modelIndex);
        matchName += "_" + std::to_string(modelIndex);
    }
    return matchName;
}

} // namespace

auto DescribeInteractiveEntities(ECS::Registry& registry, const BSPMap& map, const ImportOptions& options) -> std::vector<EntityRecord> {
    std::vector<EntityRecord> records;
    records.reserve(map.entities.size() / 4);

    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        if (className.empty()) {
            continue;
        }

        const InteractiveKind kind = ClassifyInteractive(className);
        if (kind == InteractiveKind::None) {
            continue;
        }

        EntityRecord record;
        record.kind       = kind;
        record.className  = std::string(className);
        record.targetName = std::string(entity.Find("targetname"));
        record.model      = std::string(entity.Find("model"));
        record.target     = std::string(entity.Find("target"));
        record.message    = std::string(entity.Find("message"));
        record.filterName = std::string(entity.Find("filtername"));
        record.spawnFlags = entity.FindUInt("spawnflags").value_or(0u);

        // A teleport destination has no counterpart in the scene (the scene
        // writer deliberately keeps info_* classes out of the spawned
        // entities), so it is described from the lump alone and the caller
        // creates an entity for it. Everything else is resolved against what
        // the instantiator already spawned.
        if (kind == InteractiveKind::TeleportDestination) {
            if (record.targetName.empty()) {
                continue; // unnamed destinations are not addressable
            }
            // Source stores the origin at the feet; it stays there, and the
            // teleport lifts the character by its own ground offset.
            record.origin = ConvertPosition(options, OptionalVec3(entity, "origin").value_or(JPH::Vec3::sZero()));
            // The exact conjugated Source rotation: TeleportPlayer recovers the
            // post-teleport view direction from this transform's +X axis.
            const JPH::Vec3 angles = OptionalVec3(entity, "angles").value_or(JPH::Vec3::sZero());
            record.orientation     = Math::EulerDegreesToQuat(EngineEulerAnglesFromSourceAngles(angles, options));
            records.push_back(std::move(record));
            continue;
        }

        record.matchName = record.targetName.empty() ? FallbackMatchName(className, record.model) : record.targetName;
        record.entity    = FindMatchingEntity(registry, record.matchName, record.model);
        if (record.entity == Entity::Null()) {
            continue;
        }

        if (const auto speed = entity.FindFloat("speed")) {
            record.speed = *speed;
        }
        if (const auto wait = entity.FindFloat("wait")) {
            record.wait = *wait;
        }
        if (const auto lip = entity.FindFloat("lip")) {
            record.lip = *lip;
        }
        if (const auto distance = entity.FindFloat("distance")) {
            record.distance = *distance;
        }
        if (const auto damage = entity.FindFloat("dmg")) {
            record.damage = *damage;
        }
        record.moveDirection = ParseMoveDirection(entity, options);

        record.outputs = ParseEntityOutputs(entity);

        // The `target` keyvalue is an output in its own right, with the event
        // name implied by the class: a button fires OnPressed, a trigger
        // OnTrigger, a door OnOpen. Synthesised here so the consumer sees one
        // list of outputs instead of two sources of the same wiring.
        if (!record.target.empty()) {
            EntityOutput out;
            out.target = record.target;
            out.input  = "Toggle";
            switch (kind) {
                case InteractiveKind::Button:
                case InteractiveKind::ButtonRotating: out.key = "OnPressed"; break;
                case InteractiveKind::Trigger:
                case InteractiveKind::TriggerOnce:
                case InteractiveKind::TriggerTeleport: out.key = "OnTrigger"; break;
                case InteractiveKind::Door:
                case InteractiveKind::DoorRotating: out.key = "OnOpen"; break;
                case InteractiveKind::TeleportDestination:
                case InteractiveKind::None: break;
            }
            record.outputs.push_back(std::move(out));
        }

        switch (kind) {
            case InteractiveKind::Door:
            case InteractiveKind::DoorRotating: record.door = DecodeDoorFlags(record.spawnFlags); break;
            case InteractiveKind::Button:
            case InteractiveKind::ButtonRotating: record.button = DecodeButtonFlags(record.spawnFlags); break;
            default: break;
        }

        records.push_back(std::move(record));
    }

    return records;
}

} // namespace ZHLN::BSP
