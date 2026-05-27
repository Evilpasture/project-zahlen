#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

struct Vertex {
	std::array<float, 3> pos;
	std::array<float, 3> norm;
	std::array<float, 4> tangent;
	std::array<float, 2> uv0; // For Albedo/Normal
	std::array<float, 2> uv1; // For Lightmaps
};

static_assert(std::is_trivial_v<Vertex>);

// ============================================================================
// Compile-Time Type to Vulkan Format Mapping (C++23)
// ============================================================================

template <typename T> struct FormatOf;

// Primitives
template <> struct FormatOf<float> {
	static constexpr auto value = VK_FORMAT_R32_SFLOAT;
};
template <> struct FormatOf<uint32_t> {
	static constexpr auto value = VK_FORMAT_R32_UINT;
};
template <> struct FormatOf<int32_t> {
	static constexpr auto value = VK_FORMAT_R32_SINT;
};

// Array specializations
template <> struct FormatOf<uint16_t[4]> {
	static constexpr auto value = VK_FORMAT_R16G16B16A16_UINT;
};
template <> struct FormatOf<float[4]> {
	static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};

// std::array mappings (Vec2, Vec3, Vec4)
template <> struct FormatOf<std::array<float, 2>> {
	static constexpr auto value = VK_FORMAT_R32G32_SFLOAT;
};
template <> struct FormatOf<std::array<float, 3>> {
	static constexpr auto value = VK_FORMAT_R32G32B32_SFLOAT;
};
template <> struct FormatOf<std::array<float, 4>> {
	static constexpr auto value = VK_FORMAT_R32G32B32A32_SFLOAT;
};

template <> struct FormatOf<std::array<uint32_t, 2>> {
	static constexpr auto value = VK_FORMAT_R32G32_UINT;
};
template <> struct FormatOf<std::array<uint32_t, 3>> {
	static constexpr auto value = VK_FORMAT_R32G32B32_UINT;
};
template <> struct FormatOf<std::array<uint32_t, 4>> {
	static constexpr auto value = VK_FORMAT_R32G32B32A32_UINT;
};

template <typename T> struct FormatOf {
	static_assert(sizeof(T) == 0,
				  "No Vulkan format mapping for this type. Specialize FormatOf<T>.");
};

// ============================================================================
// C++23 Consteval Helpers
// ============================================================================

struct MemberInfo {
	uint32_t offset;
	VkFormat format;
};

// Generates the binding description based on struct size
template <typename T>
[[nodiscard]] consteval auto DefaultBinding(uint32_t binding = 0) noexcept
	-> VkVertexInputBindingDescription {
	return {.binding = binding, .stride = sizeof(T), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
}

// C++20/23 Pack Expansion to perfectly initialize the attribute array
template <typename... Args> [[nodiscard]] consteval auto MakeAttributeArray(Args... args) noexcept {
	constexpr size_t count = sizeof...(Args);
	std::array<MemberInfo, count> infos{args...};
	std::array<VkVertexInputAttributeDescription, count> attrs{};

	// Compile-time loop initializes the locations sequentially (0, 1, 2...)
	for (uint32_t i = 0; i < count; ++i) {
		attrs[i] = {
			.location = i, .binding = 0, .format = infos[i].format, .offset = infos[i].offset};
	}
	return attrs;
}

// ============================================================================
// Traits and Concepts
// ============================================================================

// Base traits class (Empty by default)
template <typename T> struct VertexTraits {
	static constexpr std::array<VkVertexInputBindingDescription, 0> Bindings() { return {}; }
	static constexpr std::array<VkVertexInputAttributeDescription, 0> Attributes() { return {}; }
};

// C++20/23 Concept protecting the pipeline builder
template <typename T>
concept IsVertex = requires {
	{
		VertexTraits<T>::Bindings().data()
	} -> std::convertible_to<const VkVertexInputBindingDescription*>;
	{
		VertexTraits<T>::Attributes().data()
	} -> std::convertible_to<const VkVertexInputAttributeDescription*>;
};

} // namespace ZHLN::Vk

// ============================================================================
// Zero-Boilerplate Reflection Macros
// ============================================================================

// Extracts offset and statically deduces format using designated initializers
#define ZHLN_FIELD(Type, Mem)                                                                      \
	::ZHLN::Vk::MemberInfo {                                                                       \
		.offset = static_cast<uint32_t>(offsetof(Type, Mem)),                                      \
		.format = ::ZHLN::Vk::FormatOf<decltype(Type::Mem)>::value                                 \
	}

// --- Variadic Macro Expansion Engine (Supports up to 8 members) ---
#define ZHLN_EXPAND(x) x
#define ZHLN_GET_MACRO(_1, _2, _3, _4, _5, _6, _7, _8, NAME, ...) NAME

#define ZHLN_MAP_1(T, X) ZHLN_FIELD(T, X)
#define ZHLN_MAP_2(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_1(T, __VA_ARGS__))
#define ZHLN_MAP_3(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_2(T, __VA_ARGS__))
#define ZHLN_MAP_4(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_3(T, __VA_ARGS__))
#define ZHLN_MAP_5(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_4(T, __VA_ARGS__))
#define ZHLN_MAP_6(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_5(T, __VA_ARGS__))
#define ZHLN_MAP_7(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_6(T, __VA_ARGS__))
#define ZHLN_MAP_8(T, X, ...) ZHLN_FIELD(T, X), ZHLN_EXPAND(ZHLN_MAP_7(T, __VA_ARGS__))

#define ZHLN_MAP_MEMBERS(Type, ...)                                                                \
	ZHLN_EXPAND(ZHLN_GET_MACRO(__VA_ARGS__, ZHLN_MAP_8, ZHLN_MAP_7, ZHLN_MAP_6, ZHLN_MAP_5,        \
							   ZHLN_MAP_4, ZHLN_MAP_3, ZHLN_MAP_2, ZHLN_MAP_1)(Type, __VA_ARGS__))

/**
 * @brief Automates Vulkan binding and attribute generation for a struct.
 *
 * @param Type The C++ struct name.
 * @param ...  The members, in the exact order they map to layout(location = X).
 */
#define ZHLN_REFLECT_VERTEX(Type, ...)                                                             \
	namespace ZHLN::Vk {                                                                           \
	template <> struct VertexTraits<Type> {                                                        \
		static consteval auto Bindings() {                                                         \
			return std::array{::ZHLN::Vk::DefaultBinding<Type>(0)};                                \
		}                                                                                          \
		static consteval auto Attributes() {                                                       \
			return ::ZHLN::Vk::MakeAttributeArray(ZHLN_MAP_MEMBERS(Type, __VA_ARGS__));            \
		}                                                                                          \
	};                                                                                             \
	}
