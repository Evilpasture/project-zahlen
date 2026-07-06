#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// ============================================================================
// Compile-Time Resource Identification & Tagging
// ============================================================================

// Fixed-string wrapper for NTTP
template <size_t N> struct ResourceName {
	std::array<char, N> value{};

	// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
	constexpr ResourceName(const char (&str)[N]) { std::copy_n(str, N, value.begin()); }
};

// Represents a virtual image inside the compile-time graph
template <ResourceName Name, VkFormat Format, VkImageAspectFlags Aspect> struct GraphImage {
	static constexpr auto name = Name;
	static constexpr VkFormat format = Format;
	static constexpr VkImageAspectFlags aspect = Aspect;
};

// Represents how a pass accesses an image
template <typename Image, VkImageLayout Layout, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct Usage {
	using Resource = Image;
	static constexpr VkImageLayout layout = Layout;
	static constexpr VkPipelineStageFlags2 stage = Stage;
	static constexpr VkAccessFlags2 access = Access;
};

// Common usages
template <typename Image>
using ColorWrite =
	Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
						 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
							 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
						 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <ResourceName Name, typename UsagesTuple, typename RecordFn> struct GraphPass {
	static constexpr auto name = Name;
	using Usages = UsagesTuple;
	RecordFn record;
};

// Helper factory to create passes
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
	return GraphPass<Name, std::tuple<Usages...>, RecordFn>{std::forward<RecordFn>(record)};
}

// ============================================================================
// Compile-Time Meta-Programming & Graph Topology Discovery
// ============================================================================

template <typename... Ts> struct TypeList {
	static constexpr size_t size = sizeof...(Ts);
};

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
	using type =
		typename MergeLists<typename AppendUnique<List1, Head>::type, TypeList<Tail...>>::type;
};

template <typename UsagesTuple> struct ExtractResources;

template <typename... Usages> struct ExtractResources<std::tuple<Usages...>> {
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

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t {
	size_t index = 0;
	bool found = false;
	auto check = [&]<typename T>() {
		if (std::is_same_v<T, Target>) {
			found = true;
		}
		if (!found) {
			index++;
		}
	};
	(check.template operator()<Ts>(), ...);
	return index;
}

template <typename ResourceList, typename Target> consteval auto GetResourceIndex() -> size_t {
	constexpr size_t idx = GetResourceIndexImpl<Target>(ResourceList{});
	static_assert(idx < ResourceList::size, "Resource type is not registered in any pass.");
	return idx;
}

// ============================================================================
// Flat Compile-Time State Transition Table
// ============================================================================

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
	for (size_t r = 0; r < NumResources; ++r) {
		currentStates[r] = {.layout = VK_IMAGE_LAYOUT_UNDEFINED,
							.stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
							.access = 0};
	}

	size_t passIdx = 0;
	auto processPass = [&]<typename Pass>() {
		table[passIdx] = currentStates;

		using Usages = typename Pass::Usages;
		auto updateUsage = [&]<typename U>() {
			using Img = typename U::Resource;
			constexpr size_t rIdx = GetResourceIndex<ResourceList, Img>();
			currentStates[rIdx] = {.layout = U::layout, .stage = U::stage, .access = U::access};
		};

		[&]<size_t... Is>(std::index_sequence<Is...>) {
			(updateUsage.template operator()<std::tuple_element_t<Is, Usages>>(), ...);
		}(std::make_index_sequence<std::tuple_size_v<Usages>>{});

		passIdx++;
	};

	(processPass.template operator()<Passes>(), ...);

	return table;
}

} // namespace detail

// ============================================================================
// Type-Safe Flat Runtime Resource Bindings
// ============================================================================

struct GraphResource {
	VkImage handle = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
};

template <typename ResourceList> class ResourceBinder {
  public:
	template <typename Image> constexpr void Bind(VkImage handle, VkImageView view) noexcept {
		constexpr size_t idx = detail::GetResourceIndex<ResourceList, Image>();
		_resources[idx] = {handle, view};
	}

	constexpr auto GetBindings() const noexcept
		-> const std::array<GraphResource, ResourceList::size>& {
		return _resources;
	}

  private:
	std::array<GraphResource, ResourceList::size> _resources{};
};

// ============================================================================
// Zero-Overhead Compile-Time Frame Graph
// ============================================================================

template <typename... Passes> class CompileTimeFrameGraph {
  public:
	using Resources = typename detail::CollectAllResources<Passes...>::type;
	using Binder = ResourceBinder<Resources>;

	static constexpr size_t NumPasses = sizeof...(Passes);
	static constexpr size_t NumResources = Resources::size;

	// Computed completely once during compilation
	static constexpr auto StateTable = detail::ComputeStateTable<Resources, Passes...>();

	constexpr explicit CompileTimeFrameGraph(Passes&&... passes) : _passes(std::move(passes)...) {}

	void Execute(VkCommandBuffer cmd, const Binder& binder) const {
		const auto& bindings = binder.GetBindings();

		std::apply(
			[&](const auto&... passPack) {
				[&]<size_t... Is>(std::index_sequence<Is...>) {
					// passPack...[Is] uses native C++26 value-level pack indexing [P2662R3]
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
		constexpr size_t usageCount = std::tuple_size_v<Usages>;

		std::array<VkImageMemoryBarrier2, usageCount> barriers{};
		size_t barrierIdx = 0;

		[&]<size_t... Us>(std::index_sequence<Us...>) {
			auto buildBarrier = [&]<size_t U>() {
				using UsageType = std::tuple_element_t<U, Usages>;
				using Img = typename UsageType::Resource;

				constexpr size_t rIdx = detail::GetResourceIndex<Resources, Img>();

				// O(1) Pass-State lookup instead of O(N^2) recursive templates
				constexpr detail::ResourceState prevState = StateTable[PassIndex][rIdx];

				// O(1) Direct array access for handles instead of nested tuple searches
				const auto& resource = bindings[rIdx];

				if (prevState.layout != UsageType::layout ||
					prevState.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
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
			};
			(buildBarrier.template operator()<Us>(), ...);
		}(std::make_index_sequence<usageCount>{});

		// Submit the grouped pipeline barrier
		if (barrierIdx > 0) {
			VkDependencyInfo depInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
										.pNext = nullptr,
										.imageMemoryBarrierCount =
											static_cast<uint32_t>(barrierIdx),
										.pImageMemoryBarriers = barriers.data()};
			vkCmdPipelineBarrier2(cmd, &depInfo);
		}

		// Execute pass record lambda
		pass.record(cmd);
	}

	std::tuple<Passes...> _passes;
};

} // namespace ZHLN::Vk
