// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The engine-owned producer side of the culling output.
//
// The public read API is two `std::span<const Entity>` getters. Those say what a
// caller may do with the result -- look at it -- without saying what container
// holds it, and without handing out a reference that can clear, resize or
// reorder the engine's own state. This class is the other half: the object the
// engine and its culling systems actually write into.
//
// `std::vector` is the right storage and stays here. What had to go was the
// `Container<T>&` shape in an accessor: a mutable reference to an engine-owned
// container, which is how `engine.GetVisibleEntities().clear()` became a legal
// way to corrupt a frame, and how Jolt's `JPH::Array` ended up in a public
// header at all. A private member is not an API surface.

#include <Zahlen/ECS/Entity.hpp>
#include <cstddef>
#include <span>
#include <vector>

namespace ZHLN {

class VisibilityList {
  public:
    void Clear() noexcept;
    void Add(Entity entity);
    void AddRange(std::span<const Entity> entities);

    [[nodiscard]] auto AsSpan() const noexcept -> std::span<const Entity>;
    [[nodiscard]] auto Empty() const noexcept -> bool;
    [[nodiscard]] auto Size() const noexcept -> size_t;

  private:
    std::vector<Entity> _storage; // Hidden in implementation, NO Jolt dependency!
};

} // namespace ZHLN
