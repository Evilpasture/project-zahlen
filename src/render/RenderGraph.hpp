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

template <> struct TypeList<> {
	static constexpr size_t size = 0;

	template <size_t I> using type = void; // Safe fallback, never indexed
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
		  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT>;

template <typename Image>
using ShaderRead = Usage<Image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
						 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT>;

template <typename Image>
using DepthWrite = Usage<
	Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
	VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT>;

template <ResourceName Name, typename UsagesList, typename RecordFn_T> struct GraphPass {
	static constexpr auto name = Name;
	using Usages = UsagesList;
	using RecordFn = RecordFn_T;
	RecordFn record;
};

namespace detail {

// Bypass token for authorized framework-level render pass builders
struct BypassGraphicsCheckToken {};

// Type traits to identify rasterization/attachment writes
template <typename U> struct IsColorAttachment : std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsColorAttachment<Usage<Image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, Stage, Access>>
	: std::true_type {};

template <typename U> struct IsDepthAttachment : std::false_type {};

template <typename Image, VkPipelineStageFlags2 Stage, VkAccessFlags2 Access>
struct IsDepthAttachment<Usage<Image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, Stage, Access>>
	: std::true_type {};

// 1. ADDED: Metaprogramming list filter
template <typename InList, typename OutList, template <typename> class Predicate> struct FilterImpl;

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

template <typename List, template <typename> class Predicate>
using Filter = typename FilterImpl<List, TypeList<>, Predicate>::type;

} // namespace detail

/**
 * @brief SAFE, compile-time verified pass builder.
 * Triggers a static assertion if rasterization attachments are used directly.
 */
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePass(RecordFn&& record) {
	constexpr bool hasGraphics = (detail::IsColorAttachment<Usages>::value || ...) ||
								 (detail::IsDepthAttachment<Usages>::value || ...);

	// 2. CHANGED: Block direct raw VkCommandBuffer parameters for graphics passes
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

/**
 * @brief UNSAFE / Framework-only pass builder.
 * Required for internal wrappers that manually handle render pass boundaries.
 */
template <ResourceName Name, typename... Usages, typename RecordFn>
constexpr auto MakePassUnsafe(RecordFn&& record,
							  [[maybe_unused]] detail::BypassGraphicsCheckToken unused = {}) {
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
	[[maybe_unused]] bool found = ((std::is_same_v<Target, Ts> ? true : (++idx, false)) || ...);
	return idx;
}

// ============================================================================
// Zero-Allocation Compile-Time String Formatter
// ============================================================================

template <size_t Capacity> struct ConstexprString {
	std::array<char, Capacity> data_buffer{};
	size_t length = 0;

	constexpr void append(std::string_view sv) noexcept {
		size_t to_copy = std::min(sv.size(), Capacity - 1 - length);
		for (size_t i = 0; i < to_copy; ++i) {
			data_buffer[length + i] = sv[i];
		}
		length += to_copy;
		data_buffer[length] = '\0';
	}

	[[nodiscard]] constexpr auto string_view() const noexcept -> std::string_view {
		return std::string_view(data_buffer.data(), length);
	}
};

template <typename ResourceList, typename Target> consteval auto GetResourceIndex() -> size_t {
	constexpr size_t idx = GetResourceIndexImpl<Target>(ResourceList{});

	if constexpr (idx >= ResourceList::size) {
		// Generate a beautifully formatted, highly noticeable compile-time error block
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
	VkExtent2D extent{};
};

template <typename ResourceList> class ResourceBinder {
  public:
	template <typename Image>
	constexpr void Bind(VkImage handle, VkImageView view, VkExtent2D extent) noexcept {
		constexpr size_t idx = detail::GetResourceIndexImpl<Image>(ResourceList{});
		if constexpr (idx < ResourceList::size) {
			_resources[idx] = {handle, view, extent};
		}
	}

	constexpr auto GetBindings() const noexcept
		-> const std::array<GraphResource, ResourceList::size>& {
		return _resources;
	}

  private:
	std::array<GraphResource, ResourceList::size> _resources{};
};

// 3. ADDED: Forward declare RasterPassContext to satisfy CompileTimeFrameGraph dependencies
template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
class RasterPassContext;

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
		using RecordFn = typename PassType::RecordFn; // Query the lambda's signature
		constexpr size_t usageCount = Usages::size;

		std::array<VkImageMemoryBarrier2, usageCount> barriers{};
		size_t barrierIdx = 0;

		[&]<size_t... Us>(std::index_sequence<Us...>) {
			(
				[&]() {
					using UsageType = typename Usages::template type<Us>;
					using Img = typename UsageType::Resource;

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
				}(),
				...);
		}(std::make_index_sequence<usageCount>{});

		if (barrierIdx > 0) {
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

		using ColorWrites = detail::Filter<Usages, detail::IsColorAttachment>;
		using DepthWrites = detail::Filter<Usages, detail::IsDepthAttachment>;
		constexpr bool isGraphics = (ColorWrites::size > 0) || (DepthWrites::size > 0);

		if constexpr (isGraphics) {
			// Check if the lambda expects a raw VkCommandBuffer (Manual Pass)
			if constexpr (std::is_invocable_v<RecordFn, VkCommandBuffer>) {
				// Manual Pass: Execute raw, allowing the custom DynamicPass inside to handle
				// boundaries
				pass.record(cmd);
			} else {
				// Automatic Pass: Wrap the execution inside the automatic RasterPassContext
				RasterPassContext<Resources, ColorWrites, DepthWrites, PassIndex, Passes...> ctx(
					cmd, bindings);
				pass.record(ctx);
			}
		} else {
			// Compute/Transfer pass
			pass.record(cmd);
		}
	}

	std::tuple<Passes...> _passes;
};

// ============================================================================
// Automatic RenderPass Execution Context
// ============================================================================

// 5. ADDED: Generic fallback for Clear Colors (overridden by engine-level specializations)
template <typename Tag> struct ClearColorOf {
	static constexpr Color4 value = {.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f}; // Default: Black
};

// 6. ADDED: Non-leaking RAII RasterPassContext class template
template <typename ResourceList, typename ColorWrites, typename DepthWrites, size_t PassIndex,
		  typename... Passes>
class RasterPassContext {
  public:
	RasterPassContext(VkCommandBuffer cmd,
					  const std::array<GraphResource, ResourceList::size>& bindings) noexcept
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

	~RasterPassContext() noexcept { vkCmdEndRendering(m_cmd); }

	[[nodiscard]] VkCommandBuffer Cmd() const noexcept { return m_cmd; }
	[[nodiscard]] VkExtent2D Extent() const noexcept { return m_extent; }

  private:
	VkCommandBuffer m_cmd;
	VkExtent2D m_extent{};
	std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> m_colors{};

	template <size_t... Is, size_t... Js>
	void ResolveExtent(const std::array<GraphResource, ResourceList::size>& bindings,
					   std::index_sequence<Is...> /*unused*/,
					   std::index_sequence<Js...> /*unused*/) noexcept {
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

	template <size_t... Is>
	void BuildColorAttachments(const std::array<GraphResource, ResourceList::size>& bindings,
							   uint32_t& colorCount, std::index_sequence<Is...>) noexcept {
		(([&]() {
			 using Img = typename ColorWrites::template type<Is>;
			 constexpr size_t rIdx = detail::GetResourceIndex<ResourceList, Img>();
			 const auto& res = bindings[rIdx];

			 // Resolve loadOp dynamically from the compile-time layout history!
			 constexpr auto prevState =
				 CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][rIdx];
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
				 .clearValue = {.color = {.float32 = {clearColor.r, clearColor.g, clearColor.b,
													  clearColor.a}}}};
		 }()),
		 ...);
	}

	template <size_t... Js>
	bool BuildDepthAttachment(const std::array<GraphResource, ResourceList::size>& bindings,
							  VkRenderingAttachmentInfo& outDepth,
							  std::index_sequence<Js...> /*unused*/) noexcept {
		if constexpr (DepthWrites::size == 0) {
			return false;
		} else {
			using Img = typename DepthWrites::template type<0>;
			constexpr size_t rIdx = detail::GetResourceIndex<ResourceList, Img>();
			const auto& res = bindings[rIdx];

			constexpr auto prevState =
				CompileTimeFrameGraph<Passes...>::StateTable[PassIndex][rIdx];
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
};

/**
 * @brief Pack compile-time resource tags alongside runtime Vulkan handles.
 */
template <typename Tag> struct GraphImageRef {
	using TagType = Tag;
	VkImage handle = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkExtent2D extent{};
};

/**
 * @brief Factory helper to construct a GraphImageRef from a RenderTarget<F>
 */
template <typename Tag, typename T> constexpr auto MakeRef(const T& resource) noexcept {
	if constexpr (requires { resource.image.Handle(); }) {
		return GraphImageRef<Tag>{.handle = resource.image.Handle(),
								  .view = resource.view.Get(),
								  .extent = resource.extent};
	} else if constexpr (requires { resource.handle; }) {
		return GraphImageRef<Tag>{
			.handle = resource.handle, .view = resource.view, .extent = resource.extent};
	} else {
		static_assert(sizeof(T) == 0, "MakeRef: Unsupported resource type");
	}
}

/**
 * @brief Factory helper to construct a GraphImageRef from raw Vulkan handles (with default empty
 * extent)
 */
template <typename Tag> constexpr auto MakeRef(VkImage handle, VkImageView view) noexcept {
	return GraphImageRef<Tag>{.handle = handle, .view = view, .extent = {}};
}

/**
 * @brief Factory helper to construct a GraphImageRef from raw Vulkan handles
 */
template <typename Tag>
constexpr auto MakeRef(VkImage handle, VkImageView view, VkExtent2D extent) noexcept {
	return GraphImageRef<Tag>{.handle = handle, .view = view, .extent = extent};
}

/**
 * @brief Automates the binding of multiple resources using a fold expression
 */
template <typename BinderT, typename... Refs>
constexpr void AutoBind(BinderT& binder, const Refs&... refs) noexcept {
	(binder.template Bind<typename Refs::TagType>(refs.handle, refs.view, refs.extent), ...);
}

} // namespace ZHLN::Vk
