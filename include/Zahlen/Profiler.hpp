#pragma once
#include <cstddef>
#include <cstdint>

namespace ZHLN {

struct CullingStats {
	static inline uint32_t TotalObjects = 0;
	static inline uint32_t CulledObjects = 0;
	static inline bool EnableCulling = true; // Toggle for testing!
	static inline bool FreezeFrustum = false;
};

class Profiler {
  public:
	static void Record(const char* name, float timeMS) noexcept;

	// O(1) iteration interface for the UI loop
	using MetricCallback = void (*)(const char* name, float cpuTimeMS, float rollingAverageMS,
									const float* history, size_t historyCount, void* userData);
	static void IterateMetrics(MetricCallback callback, void* userData) noexcept;
};

// RAII Timer - Stripped of <chrono> and <string> templates
struct ScopedTimer {
	const char* name;
	uint64_t start;

	ScopedTimer(const char* n) noexcept;
	~ScopedTimer() noexcept;
};

} // namespace ZHLN

#define ZHLN_PROFILE_SCOPE(name) ZHLN::ScopedTimer _prof_timer(name)
