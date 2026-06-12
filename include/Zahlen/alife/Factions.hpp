// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <cstdint>
#include <detail/String.hpp>
#include <string_view>
#include <vector>

namespace ZHLN::ALife {

struct FactionDef {
	String32 name;
	uint32_t id;
};

class FactionRegistry {
  public:
	explicit FactionRegistry(uint32_t capacity);

	uint32_t Register(std::string_view name);
	[[nodiscard]] uint32_t GetID(std::string_view name) const;
	[[nodiscard]] const char* GetName(uint32_t id) const;

	void SetRelation(uint32_t a, uint32_t b, float value);
	[[nodiscard]] float GetRelation(uint32_t a, uint32_t b) const;

  private:
	std::vector<FactionDef> _definitions;
	std::vector<float> _relations;
	uint32_t _capacity;
};

} // namespace ZHLN::ALife