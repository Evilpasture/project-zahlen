// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ECS/Entity.hpp>
#include <cstddef>
#include <span>
#include <vector>

namespace ZHLN {

// One type-keyed render service owns both culling outputs. Read views are
// borrowed and must not be retained across mutations of their list.
class VisibilityState {
  public:
    [[nodiscard]] auto Visible() const noexcept -> std::span<const Entity> { return _visible; }
    [[nodiscard]] auto ShadowVisible() const noexcept -> std::span<const Entity> { return _shadowVisible; }

    // The source must not overlap the destination list.
    void SetVisible(std::span<const Entity> entities) { _visible.assign(entities.begin(), entities.end()); }
    void SetShadowVisible(std::span<const Entity> entities) { _shadowVisible.assign(entities.begin(), entities.end()); }

    void Reserve(std::size_t mainCap, std::size_t shadowCap) {
        _visible.reserve(mainCap);
        _shadowVisible.reserve(shadowCap);
    }
    void Clear() noexcept {
        _visible.clear();
        _shadowVisible.clear();
    }
    void AddVisible(Entity entity) { _visible.push_back(entity); }
    void AddShadowVisible(Entity entity) { _shadowVisible.push_back(entity); }

  private:
    std::vector<Entity> _visible;
    std::vector<Entity> _shadowVisible;
};

} // namespace ZHLN
