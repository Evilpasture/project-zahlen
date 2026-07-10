#include "ReflectedLayout.hpp"

#include <map>

namespace ZHLN::Vk {

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
} // namespace ZHLN::Vk
