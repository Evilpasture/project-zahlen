// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN::ALife {

struct FactionDef {
    String32 name;
    uint32_t id {};
};

class ZHLN_API FactionRegistry {
  public:
    explicit FactionRegistry(uint32_t capacity);

    auto               Register(std::string_view name) -> uint32_t;
    [[nodiscard]] auto GetID(std::string_view name) const -> uint32_t;
    [[nodiscard]] auto GetName(uint32_t id) const -> const char*;

    void               SetRelation(uint32_t a, uint32_t b, float value);
    [[nodiscard]] auto GetRelation(uint32_t a, uint32_t b) const -> float;

  private:
    std::vector<FactionDef> _definitions;
    std::vector<float>      _relations;
    uint32_t                _capacity;
};

} // namespace ZHLN::ALife
