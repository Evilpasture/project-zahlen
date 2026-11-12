// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Common.h>
// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <cstdint>
#include <vector>

// Forward declare engine registry
namespace ZHLN::ECS {
class Registry;
}

namespace ZHLN::ALife {

struct ALifeComponent;

class ZHLN_STATIC_API SpatialGrid {
  public:
    using WriteQuery = ECS::Query<ALifeComponent&>;
    using ReadQuery = ECS::Query<const ALifeComponent>;

    SpatialGrid(uint32_t w, uint32_t h, float cell_size);

    void Clear() noexcept;
    void UpdateEntity(WriteQuery query, Entity handle, DVec3 old_pos);
    void RemoveEntity(WriteQuery query, Entity handle);
    [[nodiscard]] auto Query(ReadQuery query, DVec3 pos, float radius, std::vector<Entity> reusable = {}) const -> std::vector<Entity>;

    // Compatibility overloads for standalone Registry callers.
    void UpdateEntity(ECS::Registry& reg, Entity handle, DVec3 old_pos);
    // Unlink before destroying the ECS component: its next link lives there.
    void RemoveEntity(ECS::Registry& reg, Entity handle);

    // Returns matching entities. A caller may move in a reusable buffer to retain its capacity.
    [[nodiscard]] auto Query(const ECS::Registry& reg, DVec3 pos, float radius, std::vector<Entity> reusable = {}) const
        -> std::vector<Entity>;

    [[nodiscard]] auto GetCellIndex(DVec3 pos) const noexcept -> int32_t;

    std::vector<Entity> _cellHeads;
    uint32_t            _width;
    uint32_t            _height;
    float               _cellSize;

  private:
    template <typename View>
    void UpdateEntityImpl(View& view, Entity handle, DVec3 old_pos);
    template <typename View>
    void RemoveEntityImpl(View& view, Entity handle);
    template <typename View>
    [[nodiscard]] auto QueryImpl(const View& view, DVec3 pos, float radius, std::vector<Entity> reusable) const -> std::vector<Entity>;
};

} // namespace ZHLN::ALife
