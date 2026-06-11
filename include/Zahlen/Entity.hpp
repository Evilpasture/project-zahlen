#pragma once
#include <cstdint>
#include <type_traits>

namespace ZHLN {

struct Entity {
	uint32_t index;
	uint32_t generation;

	[[nodiscard]] constexpr uint64_t Pack() const noexcept {
		return (static_cast<uint64_t>(generation) << 32) | index;
	}

	[[nodiscard]] static constexpr Entity Unpack(uint64_t raw) noexcept {
		return {.index = static_cast<uint32_t>(raw & 0xFFFFFFFF),
				.generation = static_cast<uint32_t>(raw >> 32)};
	}

	constexpr bool operator==(const Entity& other) const noexcept = default;
};

static_assert((std::is_trivially_default_constructible_v<Entity> && std::is_trivially_copyable_v<Entity>) && sizeof(Entity) == 8);

// Sentinel value
constexpr Entity NullEntity = {0xFFFFFFFF, 0xFFFFFFFF};

} // namespace ZHLN