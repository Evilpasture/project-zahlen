// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

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

#if defined(__cpp_impl_reflection) && !defined(__clang__)
// ============================================================================
// C++26 Automatic Layout Reflection Engine (Primary Template)
// ============================================================================

namespace detail {
// Pure structural array wrapper to cross boundary contexts cleanly
template <size_t N> struct MetaRange {
	std::meta::info data[N == 0 ? 1 : N]{};
};

// Calculate count on the type handle level
template <typename T> [[nodiscard]] consteval size_t GetFieldCount() noexcept {
	return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged())
		.size();
}

// Isolate data extraction into a flat array structure with an explicit compile-time size
template <typename T, size_t N> [[nodiscard]] consteval auto ExtractMetaElements() noexcept {
	auto members =
		std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged());
	MetaRange<N> range{};
	for (size_t i = 0; i < N; ++i) {
		range.data[i] = members[i];
	}
	return range;
}
} // namespace detail

template <typename T> struct AutoReflectAttributes {
	static constexpr size_t attribute_count = detail::GetFieldCount<T>();
	static constexpr auto meta_range = detail::ExtractMetaElements<T, attribute_count>();

	template <size_t... Is>
	static consteval auto generate_descriptions(std::index_sequence<Is...>) noexcept {
		return std::array<VkVertexInputAttributeDescription, attribute_count>{[]() consteval {
			constexpr auto field = meta_range.data[Is];
			using FieldType = typename[:std::meta::type_of(field):];
			return VkVertexInputAttributeDescription{
				.location = static_cast<uint32_t>(Is),
				.binding = 0,
				.format = FormatOf<FieldType>::value,
				.offset = static_cast<uint32_t>(std::meta::offset_of(field).bytes)};
		}()...};
	}

	static consteval auto get() noexcept {
		return generate_descriptions(std::make_index_sequence<attribute_count>{});
	}
};

template <typename T> struct VertexTraits {
	static consteval auto Bindings() { return std::array{DefaultBinding<T>(0)}; }
	static consteval auto Attributes() { return AutoReflectAttributes<T>::get(); }
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

#ifdef __cpp_impl_reflection
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
