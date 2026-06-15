// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#ifdef __cpp_reflection
#include <meta>
#endif
#include <type_traits>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

struct Vertex {
	std::array<float, 3> pos;
	std::array<float, 3> norm;
	std::array<float, 4> tangent;
	std::array<float, 2> uv0;
	std::array<float, 2> uv1;
};

static_assert((std::is_trivially_default_constructible_v<Vertex> &&
			   std::is_trivially_copyable_v<Vertex>));

// ============================================================================
// Type to Vulkan Format Mapping
// ============================================================================

template <typename T> struct FormatOf;

template <> struct FormatOf<float> {
	static constexpr auto value = VK_FORMAT_R32_SFLOAT;
};
template <> struct FormatOf<uint32_t> {
	static constexpr auto value = VK_FORMAT_R32_UINT;
};
template <> struct FormatOf<int32_t> {
	static constexpr auto value = VK_FORMAT_R32_SINT;
};
template <> struct FormatOf<uint16_t[4]> {
	static constexpr auto value = VK_FORMAT_R16G16B16A16_UINT;
};
template <> struct FormatOf<float[4]> {
	static constexpr auto value = VK_FORMAT_R32G32B32A32_SFLOAT;
};
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

template <typename T>
[[nodiscard]] consteval auto DefaultBinding(uint32_t binding = 0) noexcept
	-> VkVertexInputBindingDescription {
	return {.binding = binding, .stride = sizeof(T), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
}

struct MemberInfo {
	uint32_t offset;
	VkFormat format;
};

template <typename... Args> [[nodiscard]] consteval auto MakeAttributeArray(Args... args) noexcept {
	constexpr size_t count = sizeof...(Args);
	std::array<VkVertexInputAttributeDescription, count> attrs{};
	size_t i = 0;
	((attrs[i] = {.location = static_cast<uint32_t>(i),
				  .binding = 0,
				  .format = args.format,
				  .offset = args.offset},
	  ++i),
	 ...);
	return attrs;
}

#ifdef __cpp_reflection
// ============================================================================
// C++26 Automatic Layout Reflection Engine (Primary Template)
// ============================================================================

/**
 * @brief Completely automated C++26 attribute extractor matching GCC 16 specifications.
 */
template <typename T> [[nodiscard]] consteval auto ReflectAttributes() noexcept {
	constexpr auto fields = std::meta::nonstatic_data_members_of(
		std::meta::reflexpr(T), std::meta::access_context::unprivileged());
	constexpr size_t count = fields.size();

	std::array<VkVertexInputAttributeDescription, count> attrs{};

	size_t i = 0;
	template for (constexpr auto field : fields) {
		using FieldType = typename[:std::meta::type_of(field):];
		constexpr auto layout_offset = std::meta::offset_of(field);

		attrs[i] = {.location = static_cast<uint32_t>(i),
					.binding = 0,
					.format = FormatOf<FieldType>::value,
					.offset = static_cast<uint32_t>(layout_offset.bytes)};
		++i;
	}
	return attrs;
}

template <typename T> struct VertexTraits {
	static consteval auto Bindings() { return std::array{DefaultBinding<T>(0)}; }
	static consteval auto Attributes() { return ReflectAttributes<T>(); }
};

#else

// Declarations only. Specializations are registered via ZHLN_REFLECT_VERTEX.
template <typename T> struct VertexTraits;

#endif

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
// Macro Fallback & Verification Engine
// ============================================================================

#define ZHLN_FIELD(Type, Mem)                                                                      \
	::ZHLN::Vk::MemberInfo {                                                                       \
		.offset = static_cast<uint32_t>(offsetof(Type, Mem)),                                      \
		.format = ::ZHLN::Vk::FormatOf<decltype(Type::Mem)>::value                                 \
	}

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

#ifdef __cpp_reflection
#define ZHLN_REFLECT_VERTEX(Type, ...)                                                             \
	static_assert(::ZHLN::Vk::IsVertex<Type>,                                                      \
				  "Type '" #Type "' failed automatic C++26 vertex layout validation.");
#else
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
#endif
