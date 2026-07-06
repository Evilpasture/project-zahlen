// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderGraph.hpp
#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// ============================================================================
// Compile-Time Resource Identification & Tagging
// ============================================================================

template <size_t N> struct ResourceName {
	std::array<char, N> value{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
	constexpr ResourceName(const char (&str)[N]) {
		for (size_t i = 0; i < N; ++i) {
			value[i] = str[i];
		}
	}
};

// Defined early so GraphPass and MakePass can resolve it during compilation
template <typename... Ts> struct TypeList {
	static constexpr size_t size = sizeof...(Ts);

	// C++26 Pack Indexing on Types: T...[N]
	template <size_t I> using type = Ts...[I];
};

template <ResourceName Name, VkFormat Format, VkImageAspectFlags Aspect, bool IsSwapchain = false>
struct GraphImage {
	static constexpr auto name = Name;
	static constexpr VkFormat format = Format;
	static constexpr VkImageAspectFlags aspect = Aspect;
	static constexpr bool is_swapchain = IsSwapchain;
};

template <typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct Usage {
	using Resource = Image;
	static constexpr VkImageLayout layout = Layout;
	static constexpr VkPipelineStageFlags2 stage = Stage;
	static constexpr VkAccessFlags2 access = Access;
};

template <typename Image>
using ColorWrite =
	Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<
	Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
	VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <ResourceName Name, typename UsagesList, typename RecordFn> struct GraphPass {
	static constexpr auto name = Name;
	using Usages = UsagesList;
	RecordFn record;
};

// Modernized: Pack Usages directly into TypeList instead of std::tuple
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
	return GraphPass<Name, TypeList<Usages...>, RecordFn>{std::forward<RecordFn>(record)};
}

// ============================================================================
// Compile-Time Meta-Programming & Graph Topology Discovery
// ============================================================================

namespace detail {

template <typename List, typename T> struct AppendUnique;

template <typename... Ts, typename T> struct AppendUnique<TypeList<Ts...>, T> {
	static constexpr bool contains = (std::is_same_v<Ts, T> || ...);
	using type = std::conditional_t<contains, TypeList<Ts...>, TypeList<Ts..., T>>;
};

template <typename List1, typename List2> struct MergeLists;

template <typename List1> struct MergeLists<List1, TypeList<>> {
	using type = List1;
};

template <typename List1, typename Head, typename... Tail>
struct MergeLists<List1, TypeList<Head, Tail...>> {
	using HeadResources = typename AppendUnique<List1, Head>::type;
	using type = typename MergeLists<HeadResources, TypeList<Tail...>>::type;
};

template <typename UsagesList> struct ExtractResources;

template <typename... Usages> struct ExtractResources<TypeList<Usages...>> {
	using type = TypeList<typename Usages::Resource...>;
};

template <typename... Passes> struct CollectAllResources {
	using type = TypeList<>;
};

template <typename Head, typename... Tail> struct CollectAllResources<Head, Tail...> {
	using HeadResources = typename ExtractResources<typename Head::Usages>::type;
	using TailResources = typename CollectAllResources<Tail...>::type;
	using type = typename MergeLists<HeadResources, TailResources>::type;
};

// Modernized: Elegant short-circuiting fold expression finding type index
template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t {
	size_t idx = 0;
	((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
	return idx;
}

template <typename ResourceList, typename Target> consteval auto GetResourceIndex() -> size_t {
	constexpr size_t idx = GetResourceIndexImpl<Target>(ResourceList{});
	static_assert(idx < ResourceList::size, "Resource type is not registered in any pass.");
	return idx;
}

struct ResourceState {
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_NONE;
	VkAccessFlags2 access = 0;
};

template <typename ResourceList, typename... Passes> consteval auto ComputeStateTable() {
	constexpr size_t NumPasses = sizeof...(Passes);
	constexpr size_t NumResources = ResourceList::size;

	std::array<std::array<ResourceState, NumResources>, NumPasses> table{};
	std::array<ResourceState, NumResources> currentStates{};

	// Modernized: fold over indices directly
	[&]<size_t... Is>(std::index_sequence<Is...>) {
		((currentStates[Is] = {.layout = VK_IMAGE_LAYOUT_UNDEFINED,
							   .stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
							   .access = 0}),
		 ...);
	}(std::make_index_sequence<NumResources>{});

	auto simulatePasses = [&](bool record) noexcept(false) {
		size_t passIdx = 0;
		auto processPass = [&]<typename Pass>() noexcept(false) {
			if (record) {
				table[passIdx] = currentStates;
			}

			using Usages = typename Pass::Usages;

			// Modernized: Standard C++26 index fold over TypeList usages
			[&]<size_t... Is>(std::index_sequence<Is...>) {
				(
					[&]<size_t I>() {
						using U = typename Usages::template type<I>;
						using Img = typename U::Resource;
						constexpr size_t rIdx = GetResourceIndex<ResourceList, Img>();
						currentStates[rIdx] = {
							.layout = U::layout, .stage = U::stage, .access = U::access};
					}.template operator()<Is>(),
					...);
			}(std::make_index_sequence<Usages::size>{});

			passIdx++;
		};

		(processPass.template operator()<Passes>(), ...);
	};

	// First pass to simulate end-of-frame states
	simulatePasses(false);

	// Reset swapchain images to UNDEFINED (C++26 index fold + nested if constexpr)
	[&]<size_t... Is>(std::index_sequence<Is...>) {
		(
			[&]<size_t I>() {
				using R = typename ResourceList::template type<I>;
				if constexpr (R::is_swapchain) {
					currentStates[I] = {.layout = VK_IMAGE_LAYOUT_UNDEFINED,
										.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
										.access = 0};
				}
			}.template operator()<Is>(),
			...);
	}(std::make_index_sequence<NumResources>{});

	// Second pass to record actual valid looped states
	simulatePasses(true);

	return table;
}

} // namespace detail

struct GraphResource {
	VkImage handle = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

template <typename ResourceList> class ResourceBinder {
  public:
	template <typename Image> constexpr void Bind(VkImage handle, VkImageView view) noexcept {
		constexpr size_t idx = detail::GetResourceIndexImpl<Image>(ResourceList{});
		if constexpr (idx < ResourceList::size) {
			_resources[idx] = {handle, view};
		}
	}

	constexpr auto GetBindings() const noexcept
		-> const std::array<GraphResource, ResourceList::size>& {
		return _resources;
	}

  private:
	std::array<GraphResource, ResourceList::size> _resources{};
};

template <typename... Passes> class CompileTimeFrameGraph {
  public:
	using Resources = typename detail::CollectAllResources<Passes...>::type;
	using Binder = ResourceBinder<Resources>;

	static constexpr size_t NumPasses = sizeof...(Passes);
	static constexpr size_t NumResources = Resources::size;

	static constexpr auto StateTable = detail::ComputeStateTable<Resources, Passes...>();

	constexpr explicit CompileTimeFrameGraph(Passes&&... passes) : _passes(std::move(passes)...) {}

	void Execute(VkCommandBuffer cmd, const Binder& binder) const {
		const auto& bindings = binder.GetBindings();

		std::apply(
			[&](const auto&... passPack) noexcept(false) {
				[&]<size_t... Is>(std::index_sequence<Is...>) noexcept(false) {
					// C++26 Pack Indexing on Values: pack_name...[Index]
					(ExecutePass<Is>(cmd, bindings, passPack...[Is]), ...);
				}(std::make_index_sequence<NumPasses>{});
			},
			_passes);
	}

  private:
	template <size_t PassIndex, typename PassType>
	void ExecutePass(VkCommandBuffer cmd, const std::array<GraphResource, NumResources>& bindings,
					 const PassType& pass) const {
		using Usages = typename PassType::Usages;
		constexpr size_t usageCount = Usages::size;

		std::array<VkImageMemoryBarrier2, usageCount> barriers{};
		size_t barrierIdx = 0;

		[&]<size_t... Us>(std::index_sequence<Us...>) {
			(
				[&]<size_t U>() {
					using UsageType = typename Usages::template type<U>;
					using Img = typename UsageType::Resource;

					// Fix 1: Added detail:: namespace prefix
					constexpr size_t rIdx = detail::GetResourceIndex<Resources, Img>();
					constexpr detail::ResourceState prevState = StateTable[PassIndex][rIdx];
					const auto& resource = bindings[rIdx];

					constexpr VkAccessFlags2 writeMask =
						VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
						VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
						VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
						VK_ACCESS_2_MEMORY_WRITE_BIT;
					constexpr bool isWrite = (UsageType::access & writeMask) != 0;

					if (prevState.layout != UsageType::layout ||
						prevState.stage != UsageType::stage ||
						prevState.access != UsageType::access ||
						prevState.layout == VK_IMAGE_LAYOUT_UNDEFINED || isWrite) {
						barriers[barrierIdx++] = VkImageMemoryBarrier2{
							.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
							.pNext = nullptr,
							.srcStageMask = prevState.stage,
							.srcAccessMask = prevState.access,
							.dstStageMask = UsageType::stage,
							.dstAccessMask = UsageType::access,
							.oldLayout = prevState.layout,
							.newLayout = UsageType::layout,
							.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
							.image = resource.handle,
							.subresourceRange = {.aspectMask = Img::aspect,
												 .baseMipLevel = 0,
												 .levelCount = VK_REMAINING_MIP_LEVELS,
												 .baseArrayLayer = 0,
												 .layerCount = VK_REMAINING_ARRAY_LAYERS}};
					}
				}.template operator()<Us>(),
				...);
		}(std::make_index_sequence<usageCount>{});

		if (barrierIdx > 0) {
			// Fix 2: Explicitly initialize all struct members to suppress
			// -Wmissing-field-initializers
			VkDependencyInfo depInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
										.pNext = nullptr,
										.dependencyFlags = 0,
										.memoryBarrierCount = 0,
										.pMemoryBarriers = nullptr,
										.bufferMemoryBarrierCount = 0,
										.pBufferMemoryBarriers = nullptr,
										.imageMemoryBarrierCount =
											static_cast<uint32_t>(barrierIdx),
										.pImageMemoryBarriers = barriers.data()};
			vkCmdPipelineBarrier2(cmd, &depInfo);
		}

		pass.record(cmd);
	}

	std::tuple<Passes...> _passes;
};

} // namespace ZHLN::Vk
