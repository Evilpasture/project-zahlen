// gameplay/ProjectLight/Replication/ClassVocabulary.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The words the bridge uses for project-light classes and wire enumerations.
//
// Both halves of the client need these: the snapshot applier decodes a numeric
// shape or humanoid state out of a packet, and the scene passes ask whether a
// class name is something they can build. They live in one header so the two
// halves cannot drift apart about what "a part" is -- which is what happened
// when each had its own copy of the enums and its own reading of the class list.

#include <DataModel/DataModel.hpp>
#include <cstdint>
#include <string_view>

namespace ZHLN::ProjectLight {

// Classes this client builds as renderable geometry. MeshPart and Part are the
// server's names for the two; anything else is a service, a container, or
// something this client deliberately does not render.
[[nodiscard]] constexpr auto IsPartClass(std::string_view className) noexcept -> bool {
    return className == "Part" || className == "MeshPart";
}

// Classes this client builds as lights. Deliberately just the two the light
// spawner can build: a SpotLight would be built as a point light, so listing it
// here would turn "not supported yet" into "quietly wrong".
[[nodiscard]] constexpr auto IsLightClass(std::string_view className) noexcept -> bool {
    return className == "DirectionalLight" || className == "PointLight";
}

// Wire integers are the DataModel's enumeration values; an out-of-range value is
// a server this client does not understand, and falling back to the default is
// better than trusting it.
[[nodiscard]] inline auto PartShapeFromValue(int64_t value) noexcept -> PartShape {
    if (value < 0 || value > static_cast<int64_t>(PartShape::Head)) return PartShape::Cube;
    return static_cast<PartShape>(value);
}

[[nodiscard]] inline auto StateFromValue(int64_t value) noexcept -> HumanoidState {
    if (value < 0 || value > static_cast<int64_t>(HumanoidState::GettingUp)) return HumanoidState::Idle;
    return static_cast<HumanoidState>(value);
}

} // namespace ZHLN::ProjectLight
