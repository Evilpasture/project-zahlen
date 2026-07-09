// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderGraph.hpp"

namespace ZHLN::Vk {

// ============================================================================
// ResourceName Template Definitions
// ============================================================================

template <size_t N> constexpr ResourceName<N>::ResourceName(const char (&str)[N]) {
	for (size_t i = 0; i < N; ++i) {
		value[i] = str[i];
	}
}

// ============================================================================
// detail Metaprogramming & Simulation Definitions
// ============================================================================

namespace detail {

template <typename OutList, template <typename> class Predicate>
struct FilterImpl<TypeList<>, OutList, Predicate> {
	using type = OutList;
};

template <typename Head, typename... Tail, typename... Out, template <typename> class Predicate>
struct FilterImpl<TypeList<Head, Tail...>, TypeList<Out...>, Predicate> {
	using type = typename std::conditional_t<
		Predicate<Head>::value,
		FilterImpl<TypeList<Tail...>, TypeList<Out..., typename Head::Resource>, Predicate>,
		FilterImpl<TypeList<Tail...>, TypeList<Out...>, Predicate>>::type;
};

template <typename... Ts, typename T> struct AppendUnique<TypeList<Ts...>, T> {
	static constexpr bool contains = (std::is_same_v<Ts, T> || ...);
	using type = std::conditional_t<contains, TypeList<Ts...>, TypeList<Ts..., T>>;
};

template <typename List1> struct MergeLists<List1, TypeList<>> {
	using type = List1;
};

template <typename List1, typename Head, typename... Tail>
struct MergeLists<List1, TypeList<Head, Tail...>> {
	using HeadResources = typename AppendUnique<List1, Head>::type;
	using type = typename MergeLists<HeadResources, TypeList<Tail...>>::type;
};

template <typename... Usages> struct ExtractResources<TypeList<Usages...>> {
	using type = TypeList<typename Usages::Resource...>;
};

template <> struct CollectAllResources<> {
	using type = TypeList<>;
};

template <typename Head, typename... Tail> struct CollectAllResources<Head, Tail...> {
	using HeadResources = typename ExtractResources<typename Head::Usages>::type;
	using TailResources = typename CollectAllResources<Tail...>::type;
	using type = typename MergeLists<HeadResources, TailResources>::type;
};

template <typename Target, typename... Ts>
consteval auto GetResourceIndexImpl(TypeList<Ts...> /*unused*/) -> size_t {
	size_t idx = 0;
	[[maybe_unused]] bool found = ((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
	return idx;
}

template <size_t Capacity>
constexpr void ConstexprString<Capacity>::append(std::string_view sv) noexcept {
	size_t to_copy = std::min(sv.size(), Capacity - 1 - length);
	for (size_t i = 0; i < to_copy; ++i) {
		data_buffer[length + i] = sv[i];
	}
	length += to_copy;
	data_buffer[length] = '\0';
}

template <size_t Capacity>
constexpr auto ConstexprString<Capacity>::string_view() const noexcept -> std::string_view {
	return std::string_view(data_buffer.data(), length);
}

template <typename ResourceList, typename Target> consteval auto GetResourceIndex() -> size_t {
	constexpr size_t idx = GetResourceIndexImpl<Target>(ResourceList{});

	if constexpr (idx >= ResourceList::size) {
		constexpr auto errorMessage = []() consteval {
			ConstexprString<2048> msg{};
			msg.append("\n\n");
			msg.append("==========================================================================="
					   "=====\n");
			msg.append("  [GRAPH COMPILATION ERROR]\n");
			msg.append("==========================================================================="
					   "=====\n\n");
			msg.append("  The requested resource is not registered in this Frame Graph!\n\n");
			msg.append("  Missing Resource:\n");
			msg.append("    - \"");
			msg.append(std::string_view(Target::name.value.data(), Target::name.value.size()));
			msg.append("\"\n\n");
			msg.append("  Registered Resources in this Graph:\n");

			auto appendName = [&]<typename R>() {
				msg.append("    - \"");
				msg.append(std::string_view(R::name.value.data(), R::name.value.size()));
				msg.append("\"\n");
			};

			[&]<typename... Us>(TypeList<Us...>) {
				(appendName.template operator()<Us>(), ...);
			}(ResourceList{});

			msg.append("==========================================================================="
					   "=====\n\n");
			return msg;
		}();

		static_assert(idx < ResourceList::size, errorMessage.string_view());
	}

	return idx;
}

template <typename ResourceList, typename... Passes> consteval auto ComputeStateTable() {
	constexpr size_t NumPasses = sizeof...(Passes);
	constexpr size_t NumResources = ResourceList::size;

	std::array<std::array<ResourceState, NumResources>, NumPasses> table{};
	std::array<ResourceState, NumResources> currentStates{};

	// Warm up simulation state
	[&]<size_t... Is>(std::index_sequence<Is...>) {
		((currentStates[Is] = {.layout = VK_IMAGE_LAYOUT_UNDEFINED,
							   .stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
							   .access = 0,
							   .last_write_pass = 999999}),
		 ...);
	}(std::make_index_sequence<NumResources>{});

	auto simulatePasses = [&](bool record) noexcept(false) {
		size_t passIdx = 0;
		auto processPass = [&]<typename Pass>() noexcept(false) {
			if (record) {
				table[passIdx] = currentStates;
			}

			using Usages = typename Pass::Usages;

			[&]<size_t... Is>(std::index_sequence<Is...>) {
				(
					[&]<size_t I>() {
						using U = typename Usages::template type<I>;
						using Img = typename U::Resource;
						constexpr size_t rIdx = GetResourceIndex<ResourceList, Img>();

						constexpr bool isWrite = (U::access & WriteMask) != 0;

						currentStates[rIdx] = {
							.layout = U::layout,
							.stage = U::stage,
							.access = U::access,
							// Track when the resource was last modified in this frame cycle
							.last_write_pass =
								isWrite ? passIdx : currentStates[rIdx].last_write_pass};
					}.template operator()<Is>(),
					...);
			}(std::make_index_sequence<Usages::size>{});

			passIdx++;
		};

		(processPass.template operator()<Passes>(), ...);
	};

	simulatePasses(false);

	[&]<size_t... Is>(std::index_sequence<Is...>) {
		(
			[&]<size_t I>() {
				using R = typename ResourceList::template type<I>;
				if constexpr (R::is_swapchain) {
					currentStates[I] = {.layout = VK_IMAGE_LAYOUT_UNDEFINED,
										.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
										.access = 0,
										.last_write_pass = 999999};
				}
			}.template operator()<Is>(),
			...);
	}(std::make_index_sequence<NumResources>{});

	simulatePasses(true);

	return table;
}

} // namespace detail

// ============================================================================
// Pass Builder Definitions
// ============================================================================

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
	constexpr bool hasGraphics = (detail::IsColorAttachment<Usages>::value || ...) ||
								 (detail::IsDepthAttachment<Usages>::value || ...);

	if constexpr (hasGraphics) {
		static_assert(
			!std::is_invocable_v<RecordFn, VkCommandBuffer>,
			"\n\n================================================================================\n"
			"  [COMPILER ERROR] Render pass safety violation detected!\n"
			"================================================================================\n\n"
			"  Direct use of MakePass with ColorWrite or DepthWrite is not allowed.\n"
			"  Recording draw calls outside of an active Vulkan RenderPass causes undefined "
			"behaviour.\n\n"
			"  Resolution:\n"
			"    - Write your lambdas to accept 'auto& ctx' instead of raw VkCommandBuffer.\n"
			"    - The graph executor will automatically open and close the RenderPass for you.\n\n"
			"================================================================================\n");
	}

	return GraphPass<Name, TypeList<Usages...>, RecordFn>{std::forward<RecordFn>(record)};
}

template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto Passieren(RecordFn&& record,
						 [[maybe_unused]] detail::BypassGraphicsCheckToken unused) {
	return GraphPass<Name, TypeList<Usages...>, RecordFn>{std::forward<RecordFn>(record)};
}

// ============================================================================
// ResourceBinder Definition
// ============================================================================

template <typename ResourceList>
template <typename Image>
constexpr void ResourceBinder<ResourceList>::Bind(VkImage handle, VkImageView view,
												  VkExtent2D extent) noexcept {
	constexpr size_t idx = detail::GetResourceIndex<ResourceList, Image>();
	_resources[idx] = {handle, view, extent};
}

template <typename ResourceList>
constexpr auto ResourceBinder<ResourceList>::GetBindings() const noexcept
	-> const std::array<GraphResource, ResourceList::size>& {
	return _resources;
}

// ============================================================================
// CompileTimeFrameGraph Definitions
// ============================================================================

template <typename... Passes>
constexpr CompileTimeFrameGraph<Passes...>::CompileTimeFrameGraph(Passes&&... passes)
	: _passes(std::move(passes)...) {}

template <typename... Passes>
void CompileTimeFrameGraph<Passes...>::Execute(VkCommandBuffer cmd, const Binder& binder) const {
	const auto& bindings = binder.GetBindings();

	std::apply(
		[&](const auto&... passPack) noexcept(false) {
			[&]<size_t... Is>(std::index_sequence<Is...>) noexcept(false) {
				(ExecutePass<Is>(cmd, bindings, passPack...[Is]), ...);
			}(std::make_index_sequence<NumPasses>{});
		},
		_passes);
}

template <typename... Passes>
template <size_t PassIndex, typename PassType>
void CompileTimeFrameGraph<Passes...>::ExecutePass(
	VkCommandBuffer cmd, const std::array<GraphResource, NumResources>& bindings,
	const PassType& pass) const {
	using Usages = typename PassType::Usages;
	using RecordFn = typename PassType::RecordFn;

	constexpr size_t BarrierCount = CountRequiredBarriers<PassIndex, PassType>();

	if constexpr (BarrierCount > 0) {
		constexpr auto ActiveIndices = GetBarrierUsageIndices<PassIndex, PassType, BarrierCount>();
		std::array<VkImageMemoryBarrier2, BarrierCount> barriers{};

		[&]<size_t... Bs>(std::index_sequence<Bs...>) {
			(
				[&]() {
					constexpr size_t Us = ActiveIndices[Bs];
					using UsageType = typename Usages::template type<Us>;
					using Img = typename UsageType::Resource;

					constexpr size_t rIdx = detail::GetResourceIndex<Resources, Img>();
					constexpr detail::ResourceState prevState = StateTable[PassIndex][rIdx];
					const auto& resource = bindings[rIdx];

					barriers[Bs] = VkImageMemoryBarrier2{
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
				}(),
				...);
		}(std::make_index_sequence<BarrierCount>{});

		VkDependencyInfo depInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
									.pNext = nullptr,
									.dependencyFlags = 0,
									.memoryBarrierCount = 0,
									.pMemoryBarriers = nullptr,
									.bufferMemoryBarrierCount = 0,
									.pBufferMemoryBarriers = nullptr,
									.imageMemoryBarrierCount = static_cast<uint32_t>(BarrierCount),
									.pImageMemoryBarriers = barriers.data()};
		vkCmdPipelineBarrier2(cmd, &depInfo);
	}

	using ColorWrites = detail::Filter<Usages, detail::IsColorAttachment>;
	using DepthWrites = detail::Filter<Usages, detail::IsDepthAttachment>;
	constexpr bool isGraphics = (ColorWrites::size > 0) || (DepthWrites::size > 0);

	if constexpr (isGraphics) {
		if constexpr (std::is_invocable_v<RecordFn, VkCommandBuffer>) {
			pass.record(cmd);
		} else {
			RasterPassContext<Resources, ColorWrites, DepthWrites, PassIndex, Passes...> ctx(
				cmd, bindings);
			pass.record(ctx);
		}
	} else {
		pass.record(cmd);
	}
}

// ============================================================================
// RasterPassContext Definitions
// ============================================================================

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::RasterPassContext(
	VkCommandBuffer cmd, const std::array<GraphResource, ResourceList::size>& bindings) noexcept
	: m_cmd(cmd) {

	m_extent = {};
	ResolveExtent(bindings, std::make_index_sequence<ColorWrites::size>{},
				  std::make_index_sequence<DepthWrites::size>{});

	uint32_t colorCount = 0;
	BuildColorAttachments(bindings, colorCount, std::make_index_sequence<ColorWrites::size>{});

	VkRenderingAttachmentInfo depthAttachment{};
	bool hasDepth = BuildDepthAttachment(bindings, depthAttachment,
										 std::make_index_sequence<DepthWrites::size>{});

	VkRenderingInfo rendering_info = {
		.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = 0,
		.renderArea = {.offset = {.x = 0, .y = 0}, .extent = m_extent},
		.layerCount = 1,
		.viewMask = 0,
		.colorAttachmentCount = colorCount,
		.pColorAttachments = colorCount > 0 ? m_colors.data() : nullptr,
		.pDepthAttachment = hasDepth ? &depthAttachment : nullptr,
		.pStencilAttachment = nullptr,
	};

	vkCmdBeginRendering(m_cmd, &rendering_info);

	const VkViewport viewport = {.x = 0.0f,
								 .y = 0.0f,
								 .width = (float)m_extent.width,
								 .height = (float)m_extent.height,
								 .minDepth = 0.0f,
								 .maxDepth = 1.0f};
	const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = m_extent};
	vkCmdSetViewport(m_cmd, 0, 1, &viewport);
	vkCmdSetScissor(m_cmd, 0, 1, &scissor);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex,
				  Passes...>::~RasterPassContext() noexcept {
	vkCmdEndRendering(m_cmd);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
VkCommandBuffer
RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::Cmd()
	const noexcept {
	return m_cmd;
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
VkExtent2D RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::Extent()
	const noexcept {
	return m_extent;
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
template <size_t... Is, size_t... Js>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::ResolveExtent(
	const std::array<GraphResource, ResourceList::size>& bindings,
	std::index_sequence<Is...> /*unused*/, std::index_sequence<Js...> /*unused*/) noexcept {
	(([&]() {
		 if (m_extent.width == 0) {
			 using Img = typename ColorWrites::template type<Is>;
			 m_extent = bindings[detail::GetResourceIndex<ResourceList, Img>()].extent;
		 }
	 }()),
	 ...);

	if (m_extent.width == 0) {
		(([&]() {
			 if (m_extent.width == 0) {
				 using Img = typename DepthWrites::template type<Js>;
				 m_extent = bindings[detail::GetResourceIndex<ResourceList, Img>()].extent;
			 }
		 }()),
		 ...);
	}
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
template <size_t... Is>
void RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::
	BuildColorAttachments(const std::array<GraphResource, ResourceList::size>& bindings,
						  uint32_t& colorCount, std::index_sequence<Is...> /*unused*/) noexcept {
	(([&]() {
		 using Img = typename ColorWrites::template type<Is>;
		 constexpr size_t rIdx = detail::GetResourceIndex<ResourceList, Img>();
		 const auto& res = bindings[rIdx];

		 constexpr auto prevState = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][rIdx];
		 VkAttachmentLoadOp loadOp = (prevState.layout == VK_IMAGE_LAYOUT_UNDEFINED)
										 ? VK_ATTACHMENT_LOAD_OP_CLEAR
										 : VK_ATTACHMENT_LOAD_OP_LOAD;

		 constexpr Color4 clearColor = ClearColorOf<Img>::value;

		 m_colors[colorCount++] = {
			 .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
			 .pNext = nullptr,
			 .imageView = res.view,
			 .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			 .resolveMode = VK_RESOLVE_MODE_NONE,
			 .resolveImageView = VK_NULL_HANDLE,
			 .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			 .loadOp = loadOp,
			 .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			 .clearValue = {
				 .color = {.float32 = {clearColor.r, clearColor.g, clearColor.b, clearColor.a}}}};
	 }()),
	 ...);
}

template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
template <size_t... Js>
bool RasterPassContext<ResourceList, ColorWrites, DepthWrites, PassIndex, Passes...>::
	BuildDepthAttachment(const std::array<GraphResource, ResourceList::size>& bindings,
						 VkRenderingAttachmentInfo& outDepth,
						 std::index_sequence<Js...> /*unused*/) noexcept {
	if constexpr (DepthWrites::size == 0) {
		return false;
	} else {
		using Img = typename DepthWrites::template type<0>;
		constexpr size_t rIdx = detail::GetResourceIndex<ResourceList, Img>();
		const auto& res = bindings[rIdx];

		constexpr auto prevState = CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][rIdx];
		VkAttachmentLoadOp loadOp = (prevState.layout == VK_IMAGE_LAYOUT_UNDEFINED)
										? VK_ATTACHMENT_LOAD_OP_CLEAR
										: VK_ATTACHMENT_LOAD_OP_LOAD;

		outDepth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
					.pNext = nullptr,
					.imageView = res.view,
					.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
					.resolveMode = VK_RESOLVE_MODE_NONE,
					.resolveImageView = VK_NULL_HANDLE,
					.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.loadOp = loadOp,
					.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
					.clearValue = {.depthStencil = {.depth = 1.0f, .stencil = 0}}};
		return true;
	}
}

// ============================================================================
// Factory Helper Definitions
// ============================================================================

template <typename Tag, typename T> constexpr auto MakeRef(const T& resource) noexcept {
	if constexpr (requires { resource.image.Handle(); }) {
		return GraphImageRef<Tag>{.handle = resource.image.Handle(),
								  .view = resource.view.Get(),
								  .extent = resource.extent};
	} else if constexpr (requires { resource.handle; }) {
		return GraphImageRef<Tag>{
			.handle = resource.handle, .view = resource.view, .extent = resource.extent};
	} else {
		static_assert(sizeof(T) == 0, "Unsupported resource type while making a graph reference");
	}
}

template <typename Tag> constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept {
	return GraphImageRef<Tag>{.handle = handle, .view = view, .extent = {}};
}

template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent) noexcept {
	return GraphImageRef<Tag>{.handle = handle, .view = view, .extent = extent};
}

template <typename BinderT, typename... Refs>
constexpr void AutoBind(BinderT& binder, const Refs&... refs) noexcept {
	(binder.template Bind<typename Refs::TagType>(refs.handle, refs.view, refs.extent), ...);
}

} // namespace ZHLN::Vk

// ============================================================================
// Debug Tools & Compile-Time Inspection Implementations
// ============================================================================

namespace ZHLN::Vk::Debug {

// Compile-time trait to check if a resource is referenced in a pass's usages
namespace {
template <typename UsagesList, typename Target> struct IsResourceInUsages {
	static constexpr bool value = false;
};
} // namespace

template <typename... Us, typename Target> struct IsResourceInUsages<TypeList<Us...>, Target> {
	static constexpr bool value = (std::is_same_v<typename Us::Resource, Target> || ...);
};

template <size_t Capacity>
constexpr void VisualizerString<Capacity>::append(std::string_view sv) noexcept {
	size_t to_copy = std::min(sv.size(), Capacity - 1 - length);
	for (size_t i = 0; i < to_copy; ++i) {
		data_buffer[length + i] = sv[i];
	}
	length += to_copy;
	data_buffer[length] = '\0';
}

template <size_t Capacity>
constexpr void VisualizerString<Capacity>::append_int(size_t val) noexcept {
	if (val == 0) {
		append("0");
		return;
	}
	std::array<char, 24> temp{};
	size_t i = 0;
	while (val > 0 && i < 23) {
		temp[i++] = '0' + (val % 10);
		val /= 10;
	}
	for (size_t j = 0; j < i / 2; ++j) {
		std::swap(temp[j], temp[i - 1 - j]);
	}
	append(std::string_view(temp.data(), i));
}

template <size_t Capacity>
constexpr auto VisualizerString<Capacity>::string_view() const noexcept -> std::string_view {
	return std::string_view(data_buffer.data(), length);
}

template <typename... Passes>
constexpr std::string_view
GraphVisualizer<CompileTimeFrameGraph<Passes...>>::LayoutToString(VkImageLayout layout) {
	switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			return "UNDEFINED";
		case VK_IMAGE_LAYOUT_GENERAL:
			return "GENERAL";
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return "COLOR_ATTACH_OPTIMAL";
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return "DEPTH_STENCIL_ATTACH_OPTIMAL";
		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return "SHADER_READ_ONLY";
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			return "TRANSFER_SRC";
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			return "TRANSFER_DST";
		case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
			return "DEPTH_ATTACH_OPTIMAL";
		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			return "PRESENT_SRC_KHR";
		default:
			return "UNKNOWN_OR_CUSTOM_LAYOUT";
	}
}

template <typename... Passes>
constexpr std::string_view
GraphVisualizer<CompileTimeFrameGraph<Passes...>>::StageToString(VkPipelineStageFlags2 stage) {
	if (stage == VK_PIPELINE_STAGE_2_NONE) {
		return "NONE";
	}
	if (stage == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) {
		return "TOP_OF_PIPE";
	}
	if (stage == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) {
		return "BOTTOM_OF_PIPE";
	}

	if (stage & VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) {
		return "COLOR_ATTACHMENT_OUTPUT";
	}
	if (stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) {
		return "FRAGMENT_SHADER";
	}
	if (stage & (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
				 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)) {
		return "DEPTH_STENCIL_TESTS";
	}
	if (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
		return "COMPUTE_SHADER";
	}
	if (stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) {
		return "TRANSFER";
	}

	return "COMBINED_OR_OTHER_STAGES";
}

template <typename... Passes>
constexpr std::string_view
GraphVisualizer<CompileTimeFrameGraph<Passes...>>::AccessToString(VkAccessFlags2 access) {
	if (access == 0) {
		return "NONE";
	}
	if (access & VK_ACCESS_2_SHADER_WRITE_BIT) {
		return "SHADER_WRITE";
	}
	if (access & VK_ACCESS_2_SHADER_READ_BIT) {
		return "SHADER_READ";
	}
	if (access & VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT) {
		return "COLOR_WRITE";
	}
	if (access & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) {
		return "DEPTH_WRITE";
	}
	if (access & VK_ACCESS_2_TRANSFER_WRITE_BIT) {
		return "TRANSFER_WRITE";
	}
	if (access & VK_ACCESS_2_TRANSFER_READ_BIT) {
		return "TRANSFER_READ";
	}
	return "COMBINED_OR_OTHER_ACCESS";
}

template <typename... Passes>
consteval auto GraphVisualizer<CompileTimeFrameGraph<Passes...>>::Visualize() {
	VisualizerString<32768> msg{}; // Bypassed original limit with 32KB buffer
	msg.append("\n\n");
	msg.append(
		"================================================================================\n");
	msg.append("  [RENDER GRAPH COMPILE-TIME STATE TABLE VISUALIZATION]\n");
	msg.append(
		"================================================================================\n\n");

	msg.append("  Total Registered Resources: ");
	msg.append_int(NumResources);
	msg.append("\n");
	msg.append("  Total Passes: ");
	msg.append_int(NumPasses);
	msg.append("\n\n");

	msg.append("  --- REGISTERED RESOURCES ---\n");
	size_t rIdx = 0;
	auto printResource = [&]<typename R>() {
		msg.append("    [Resource ");
		msg.append_int(rIdx);
		msg.append("]: \"");
		msg.append(std::string_view(R::name.value.data()));
		msg.append("\"");
		if constexpr (R::is_swapchain) {
			msg.append(" (SWAPCHAIN)");
		}
		msg.append("\n");
		rIdx++;
	};
	[&]<typename... Us>(TypeList<Us...>) {
		(printResource.template operator()<Us>(), ...);
	}(Resources{});

	msg.append("\n  --- PASS STATE TRANSITIONS (AT ENTRY TO PASS) ---\n");

	size_t passIdx = 0;
	auto printPass = [&]<typename Pass>() {
		msg.append("  ● Pass [");
		msg.append_int(passIdx);
		msg.append("]: \"");
		msg.append(std::string_view(Pass::name.value.data()));
		msg.append("\"\n");

		size_t resIdx = 0;
		auto printResourceState = [&]<typename Res>() {
			// Check if this pass actively references this resource
			constexpr bool isActive = IsResourceInUsages<typename Pass::Usages, Res>::value;

			if constexpr (isActive) {
				const auto state = GraphT::StateTable[passIdx][resIdx];
				msg.append("      ↳ Resource [");
				msg.append_int(resIdx);
				msg.append("] (\"");
				msg.append(std::string_view(Res::name.value.data()));
				msg.append("\")\n");

				msg.append("          Layout: ");
				msg.append(LayoutToString(state.layout));
				msg.append("\n");

				msg.append("          Stage : ");
				msg.append(StageToString(state.stage));
				msg.append("\n");

				msg.append("          Access: ");
				msg.append(AccessToString(state.access));
				msg.append("\n");
			}
			resIdx++;
		};

		[&]<typename... Us>(TypeList<Us...>) {
			(printResourceState.template operator()<Us>(), ...);
		}(Resources{});

		msg.append("\n");
		passIdx++;
	};

	(printPass.template operator()<Passes>(), ...);

	msg.append(
		"================================================================================\n\n");
	return msg;
}

} // namespace ZHLN::Vk::Debug
