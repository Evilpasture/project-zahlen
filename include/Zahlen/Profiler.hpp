#pragma once
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace ZHLN {
struct ProfileData {
	float cpuTimeMS = 0.0f;
	float rollingAverageMS = 0.0f;
	std::vector<float> history;
};

struct CullingStats {
	static inline uint32_t TotalObjects = 0;
	static inline uint32_t CulledObjects = 0;
	static inline bool EnableCulling = true; // Toggle for testing!
};

class Profiler {
  public:
	static void Record(const std::string& name, float timeMS) {
		auto& data = s_Metrics[name];
		data.cpuTimeMS = timeMS;

		// Rolling average (Lerp towards new value)
		data.rollingAverageMS = data.rollingAverageMS * 0.95f + timeMS * 0.05f;

		data.history.push_back(timeMS);
		if (data.history.size() > 100)
			data.history.erase(data.history.begin());
	}

	static const std::map<std::string, ProfileData>& GetMetrics() { return s_Metrics; }

  private:
	static inline std::map<std::string, ProfileData> s_Metrics;
};

// RAII Timer
struct ScopedTimer {
	std::string name;
	std::chrono::high_resolution_clock::time_point start;
	ScopedTimer(const std::string& n) : name(n), start(std::chrono::high_resolution_clock::now()) {}
	~ScopedTimer() {
		auto end = std::chrono::high_resolution_clock::now();
		float duration = std::chrono::duration<float, std::milli>(end - start).count();
		Profiler::Record(name, duration);
	}
};
} // namespace ZHLN

#define ZHLN_PROFILE_SCOPE(name) ZHLN::ScopedTimer _prof_timer(name)