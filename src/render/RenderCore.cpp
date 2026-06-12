// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "RenderCore.hpp"

#include "Allocator.hpp"
#include "StagingContext.hpp"

#include <cstdlib>
#include <fstream>
#include <print>

namespace ZHLN::Vk {

void ReportVkError(VkResult result, const char* context, const std::source_location& location) {
	std::println(stderr, "[Vk Error] {}:{} in {}: {} failed with {}", location.file_name(),
				 location.line(), location.function_name(), context, ZHLN_VkResultString(result));
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
	return {device, stages};
}

} // namespace ZHLN::Vk
