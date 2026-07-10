#pragma once

namespace ZHLN::Vk {

// ============================================================================
// ShaderStages RAII
// ============================================================================

[[nodiscard]] constexpr auto CreateShaderDesc(const uint32_t* code, size_t size,
											  const char* entry = nullptr) noexcept
	-> ZHLN_ShaderDesc {
	return ZHLN_ShaderDesc{.code = code, .size = size, .entry_point = entry};
}

template <typename T, size_t Extent>
[[nodiscard]] constexpr auto CreateShaderDesc(std::span<T, Extent> codeSpan,
											  const char* entry = nullptr) noexcept
	-> ZHLN_ShaderDesc {
	return ZHLN_ShaderDesc{.code = std::bit_cast<const uint32_t*>(codeSpan.data()),
						   .size = codeSpan.size_bytes(),
						   .entry_point = entry};
}

class ShaderStages {
  public:
	constexpr ShaderStages() noexcept = default;
	constexpr ShaderStages(const VkDevice device, const ZHLN_ShaderStages raw) noexcept
		: _device(device), _raw(raw) {}

	~ShaderStages() noexcept;
	ShaderStages(const ShaderStages&) = delete;
	auto operator=(const ShaderStages&) -> ShaderStages& = delete;
	ShaderStages(ShaderStages&& other) noexcept;
	auto operator=(ShaderStages&& other) noexcept -> ShaderStages&;

	[[nodiscard("Shader creation may fail; verify validity before binding")]]
	static auto Create(const VkDevice device, const ZHLN_ShaderDesc& vert,
					   const ZHLN_ShaderDesc& frag) noexcept -> ShaderStages;

	template <typename T, size_t Extent1, typename U = const uint8_t,
			  size_t Extent2 = std::dynamic_extent>
	[[nodiscard]] static auto Create(const VkDevice device, std::span<T, Extent1> vertSpan,
									 std::span<U, Extent2> fragSpan = {},
									 const char* vertEntry = nullptr,
									 const char* fragEntry = nullptr) noexcept -> ShaderStages {
		return Create(device, CreateShaderDesc(vertSpan, vertEntry),
					  fragSpan.empty() ? ZHLN_ShaderDesc{} : CreateShaderDesc(fragSpan, fragEntry));
	}

	template <typename T>
	[[nodiscard]] static auto Create(const VkDevice device, const T& pair,
									 const char* vertEntry = nullptr,
									 const char* fragEntry = nullptr) noexcept -> ShaderStages {
		return Create(device, CreateShaderDesc(pair.vertex, vertEntry),
					  pair.fragment.empty() ? ZHLN_ShaderDesc{}
											: CreateShaderDesc(pair.fragment, fragEntry));
	}

	[[nodiscard("Shader loading from files may fail; verify validity before use")]]
	static auto FromFiles(VkDevice device, const std::filesystem::path& vert_path,
						  const std::filesystem::path& frag_path, const char* vert_entry = "main",
						  const char* frag_entry = "main") noexcept -> ShaderStages;

	[[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_ShaderStages* { return &_raw; }
	[[nodiscard("Always verify shader stages are valid before pipeline creation")]]
	constexpr auto Valid() const noexcept -> bool {
		return _raw.vert.handle != VK_NULL_HANDLE;
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_ShaderStages _raw{};
};

[[nodiscard]] constexpr auto AsSpirV(const void* data) noexcept -> const uint32_t* {
	return std::bit_cast<const uint32_t*>(data);
}
} // namespace ZHLN::Vk
