#pragma once

#include <detail/Atomic.hpp>
#include <threading/Mutex.hpp>

namespace ZHLN {
struct alignas(64) BufferSync {
	ZHLN::Atomic<int> viewExportCount;
	ZHLN::Mutex shadowLock;
};

static_assert(std::is_trivial_v<BufferSync>);
} // namespace ZHLN