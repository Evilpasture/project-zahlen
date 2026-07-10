#pragma once

namespace ZHLN::Vk {

template <Vk::QueueType QType> class CommandPool {
  public:
	CommandPool() = default;
	CommandPool(const VkDevice device, const uint32_t queue_family);
	~CommandPool();

	CommandPool(const CommandPool&) = delete;
	auto operator=(const CommandPool&) -> CommandPool& = delete;

	constexpr CommandPool(CommandPool&& other) noexcept;
	auto operator=(CommandPool&& other) noexcept -> CommandPool&;

	[[nodiscard]] constexpr auto Valid() const noexcept -> bool {
		return _device != VK_NULL_HANDLE;
	}
	constexpr explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] constexpr operator const ZHLN_CommandPool&() const noexcept { return _raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool&() noexcept { return _raw; }

	[[nodiscard]] auto Allocate(const uint32_t count) -> bool;
	[[nodiscard]] auto AllocateSecondary(const uint32_t count) -> bool;
	void Reset() noexcept;

	// This is where the compiler-enforced safety is introduced!
	[[nodiscard]] constexpr auto operator[](const uint32_t idx) const noexcept
		-> Vk::CommandBuffer<QType> {
		return Vk::CommandBuffer<QType>{_raw.buffers[idx]};
	}

	[[nodiscard]] constexpr operator const ZHLN_CommandPool*() const noexcept { return &_raw; }
	[[nodiscard]] constexpr operator ZHLN_CommandPool*() noexcept { return &_raw; }

  private:
	VkDevice _device = VK_NULL_HANDLE;
	ZHLN_CommandPool _raw{};
};

template <typename... Args> CommandPool(Args&&...) -> CommandPool<QueueType::Graphics>;

template <uint32_t N, Vk::QueueType QType = Vk::QueueType::Graphics>
	requires(N > 0 && N <= 8)
class CommandPools {
  public:
	struct Description {
		uint32_t queue_family = 0;
		uint32_t buffers_per_pool = 1;
	};

	CommandPools() noexcept = default;

	[[nodiscard]] static auto Create(const VkDevice device, const Description& desc) noexcept
		-> CommandPools;

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) noexcept -> CommandPool<QType>& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept
		-> const CommandPool<QType>& {
		return _pools[frame % N];
	}

	[[nodiscard]] constexpr auto Cmd(const uint32_t frame) const noexcept
		-> Vk::CommandBuffer<QType> {
		return _pools[frame % N][0];
	}

	[[nodiscard]] constexpr auto Valid() const noexcept -> bool { return _pools[0].Valid(); }

  private:
	std::array<CommandPool<QType>, N> _pools = {};
};
} // namespace ZHLN::Vk

#include "CommandPool.inl"
