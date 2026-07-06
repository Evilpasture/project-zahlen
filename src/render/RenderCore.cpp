// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "RenderCore.hpp"

#include "spirv_reflect.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <print>

namespace ZHLN::Vk {

VkInstance CreateInstance(std::string_view appName, uint32_t appVersion,
						  std::span<const std::string_view> extensions,
						  bool enableValidation) noexcept {
	std::vector<const char*> c_strings;
	c_strings.reserve(extensions.size());
	for (const auto& sv : extensions) {
		c_strings.push_back(sv.data());
	}

	ZHLN_InstanceDesc inst_desc = {.app_name = {},
								   .version = appVersion,
								   .extension_count = static_cast<uint32_t>(c_strings.size()),
								   .severity_flags =
									   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
									   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
								   .extensions = c_strings.data(),
								   .enable_validation = enableValidation};

	const size_t copy_size = ZHLN::Min(appName.size(), sizeof(inst_desc.app_name) - 1);
	std::memcpy(inst_desc.app_name, appName.data(), copy_size);
	inst_desc.app_name[copy_size] = '\0';

	return ZHLN_CreateInstance(&inst_desc);
}

ZHLN_PhysicalDeviceInfo SelectDevice(VkInstance instance, VkSurfaceKHR surface) noexcept {
	ZHLN_DeviceSelectDesc select_desc = {
		.instance = instance, .surface = surface, .score_fn = nullptr, .score_userdata = nullptr};
	return ZHLN_SelectPhysicalDevice(&select_desc);
}

std::string ReportVkError(VkResult result, const char* context,
						  const std::source_location& location = std::source_location::current()) {
	return std::format("[Vk Error] {}:{} in {}: {} failed with {}", location.file_name(),
					   location.line(), location.function_name(), context,
					   ZHLN_VkResultString(result));
}

[[noreturn]] void ReportSemaphoreBoundsError(uint32_t index, uint32_t count) noexcept {
	std::println(stderr, "[ZHLN::Vk] FATAL: SemaphorePool index {} out of bounds (Size: {})", index,
				 count);
	std::abort();
}

ShaderStages::~ShaderStages() noexcept {
	if (_device != VK_NULL_HANDLE) {
		ZHLN_DestroyShaderStages(_device, &_raw);
	}
}

ShaderStages::ShaderStages(ShaderStages&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {}

auto ShaderStages::operator=(ShaderStages&& other) noexcept -> ShaderStages& {
	if (this != &other) {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyShaderStages(_device, &_raw);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_raw = std::exchange(other._raw, {});
	}
	return *this;
}

auto ShaderStages::FromFiles(const VkDevice device, const std::filesystem::path& vert_path,
							 const std::filesystem::path& frag_path, const char* vert_entry,
							 const char* frag_entry) noexcept -> ShaderStages {
	auto load = [](const std::filesystem::path& path) -> std::vector<uint32_t> {
		if (path.empty()) {
			return {};
		}
		std::ifstream file(path, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			return {};
		}
		const std::streamsize size = file.tellg();
		std::vector<uint32_t> buffer(size / sizeof(uint32_t));
		file.seekg(0);
		file.read(reinterpret_cast<char*>(buffer.data()), size);
		return buffer;
	};

	auto vert_spv = load(vert_path);
	auto frag_spv = load(frag_path);

	if (vert_spv.empty() || (!frag_path.empty() && frag_spv.empty())) {
		std::println(stderr, "[ZHLN::Vk] Failed to load shader files: {} or {}", vert_path.string(),
					 frag_path.string());
		return {};
	}

	const ZHLN_ShaderDesc v_desc = {
		.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = vert_entry};
	const ZHLN_ShaderDesc f_desc = {
		.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = frag_entry};

	return Create(device, v_desc, f_desc);
}

auto ShaderStages::Create(const VkDevice device, const ZHLN_ShaderDesc& vert,
						  const ZHLN_ShaderDesc& frag) noexcept -> ShaderStages {
	const ZHLN_ShaderStagesDesc desc = {.device = device, .vert = vert, .frag = frag};
	ZHLN_ShaderStages stages{};
	if (!ZHLN_CreateShaderStages(&desc, &stages)) {
		return {};
	}
	stages.vert.view_mask = ZHLN_DetectShaderViewMask(&vert);
	stages.frag.view_mask = ZHLN_DetectShaderViewMask(&frag);
	return {device, stages};
}

void UnsafeReflectedLayoutBuilder::AddStageUnsafe(const ZHLN_ShaderDesc& desc,
												  VkShaderStageFlags stage) noexcept {
	if ((desc.code != nullptr) && desc.size > 0 && _stageCount < 5) {
		_stages[_stageCount++] = {.code = desc.code, .size = desc.size, .stage = stage};
	}
}

auto UnsafeReflectedLayoutBuilder::BuildUnsafe(VkDevice device) noexcept -> UnsafeReflectedLayout {
	UnsafeReflectedLayout result;

	// Sorted map: SetIndex -> Map: BindingIndex -> VkDescriptorSetLayoutBinding
	std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> mergedSets;
	std::map<uint32_t, std::map<uint32_t, VkDescriptorBindingFlags>> mergedFlags;
	std::map<uint32_t, VkPushConstantRange> mergedPushConstants;

	for (uint32_t i = 0; i < _stageCount; ++i) {
		const auto& stage = _stages[i];
		SpvReflectShaderModule module;
		SpvReflectResult reflectResult =
			spvReflectCreateShaderModule(stage.size, stage.code, &module);
		if (reflectResult != SPV_REFLECT_RESULT_SUCCESS) {
			continue;
		}

		uint32_t setCount = 0;
		spvReflectEnumerateDescriptorSets(&module, &setCount, nullptr);
		std::vector<SpvReflectDescriptorSet*> sets(setCount);
		spvReflectEnumerateDescriptorSets(&module, &setCount, sets.data());

		for (const auto* reflectedSet : sets) {
			uint32_t setIdx = reflectedSet->set;

			for (uint32_t b = 0; b < reflectedSet->binding_count; ++b) {
				const auto* reflectedBinding = reflectedSet->bindings[b];
				uint32_t bindingIdx = reflectedBinding->binding;

				auto& binding = mergedSets[setIdx][bindingIdx];
				binding.binding = bindingIdx;
				binding.descriptorType =
					static_cast<VkDescriptorType>(reflectedBinding->descriptor_type);
				binding.descriptorCount = reflectedBinding->count;
				binding.stageFlags |= stage.stage;
				binding.pImmutableSamplers = nullptr;

				// ACCUMULATE EXACT TYPE COUNT FOR THE POOL
				result.descriptorTypeCounts[binding.descriptorType] += binding.descriptorCount;

				if (reflectedBinding->count >= 1024) {
					mergedFlags[setIdx][bindingIdx] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
													  VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
				}
			}
		}

		uint32_t pushCount = 0;
		spvReflectEnumeratePushConstantBlocks(&module, &pushCount, nullptr);
		std::vector<SpvReflectBlockVariable*> pushBlocks(pushCount);
		spvReflectEnumeratePushConstantBlocks(&module, &pushCount, pushBlocks.data());

		for (const auto* block : pushBlocks) {
			uint32_t offset = block->offset;
			auto& range = mergedPushConstants[offset];
			range.offset = offset;
			range.size = std::max(range.size, block->size);
			range.stageFlags |= stage.stage;
		}

		spvReflectDestroyShaderModule(&module);
	}

	// Create Descriptor Set Layouts (Capped at 4)
	uint32_t setLayoutCount = 0;
	for (const auto& [setIdx, bindingsMap] : mergedSets) {
		if (setLayoutCount >= 4) {
			break;
		}

		std::vector<VkDescriptorSetLayoutBinding> bindings;
		std::vector<VkDescriptorBindingFlags> bindingFlags;
		bool hasBindlessFlags = false;

		for (const auto& [bindingIdx, binding] : bindingsMap) {
			bindings.push_back(binding);

			VkDescriptorBindingFlags flags = mergedFlags[setIdx][bindingIdx];
			bindingFlags.push_back(flags);
			if (flags != 0) {
				hasBindlessFlags = true;
			}
		}

		const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
			.pNext = nullptr,
			.bindingCount = static_cast<uint32_t>(bindingFlags.size()),
			.pBindingFlags = bindingFlags.data()};

		const VkDescriptorSetLayoutCreateInfo createInfo = {
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
			.pNext = hasBindlessFlags ? &flagsInfo : nullptr,
			.flags = hasBindlessFlags
						 ? (VkDescriptorSetLayoutCreateFlags)
							   VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT
						 : 0u,
			.bindingCount = static_cast<uint32_t>(bindings.size()),
			.pBindings = bindings.data()};

		VkDescriptorSetLayout layout = VK_NULL_HANDLE;
		vkCreateDescriptorSetLayout(device, &createInfo, nullptr, &layout);

		result.descriptorSetLayouts[setLayoutCount] = DescriptorSetLayout(device, layout);
		setLayoutCount++;
	}
	result.setLayoutCount = setLayoutCount;

	// Flatten Push Constant Ranges
	std::vector<VkPushConstantRange> pushRanges;
	pushRanges.reserve(mergedPushConstants.size());
	for (const auto& [offset, range] : mergedPushConstants) {
		pushRanges.push_back(range);
	}

	// Extract raw layouts
	std::vector<VkDescriptorSetLayout> rawSetLayouts(setLayoutCount);
	for (uint32_t i = 0; i < setLayoutCount; ++i) {
		rawSetLayouts[i] = result.descriptorSetLayouts[i].Get();
	}

	// Create Pipeline Layout
	const VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pNext = nullptr,
		.flags = 0,
		.setLayoutCount = setLayoutCount,
		.pSetLayouts = rawSetLayouts.data(),
		.pushConstantRangeCount = static_cast<uint32_t>(pushRanges.size()),
		.pPushConstantRanges = pushRanges.data()};

	VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
	vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
	result.pipelineLayout = PipelineLayout(device, pipelineLayout);

	return result;
}

bool RayTracingContext::Init(VkDevice device) noexcept {
	bool ok = ZHLN_InitRayTracingContext(device, &_raw);
	if (!ok) {
		_raw.device = VK_NULL_HANDLE;
	}
	return ok;
}

void SubmitAndWait(VkQueue queue, VkCommandBuffer cmd, VkSemaphore waitSemaphore,
				   uint64_t waitValue, VkPipelineStageFlags2 waitStage) noexcept {
	VkCommandBufferSubmitInfo cmdInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.pNext = {},
		.commandBuffer = cmd,
		.deviceMask = {},
	};

	VkSemaphoreSubmitInfo waitInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.pNext = {},
		.semaphore = waitSemaphore,
		.value = waitValue,
		.stageMask = waitStage,
		.deviceIndex = {},
	};

	uint32_t waitCount = (waitSemaphore != VK_NULL_HANDLE && waitValue > 0) ? 1 : 0;

	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.pNext = {},
		.flags = {},
		.waitSemaphoreInfoCount = waitCount,
		.pWaitSemaphoreInfos = waitCount > 0 ? &waitInfo : nullptr,
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmdInfo,
		.signalSemaphoreInfoCount = {},
		.pSignalSemaphoreInfos = {},
	};

	vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(queue);
}

} // namespace ZHLN::Vk
