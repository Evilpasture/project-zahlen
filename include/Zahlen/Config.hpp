#pragma once
#include <cstdint>
#include <detail/String.hpp>

namespace ZHLN {

struct PhysicsConfig {
	uint32_t maxBodies = 1024;
	uint32_t maxBodyPairs = 1024;
	uint32_t maxContactConstraints = 1024;
	uint32_t tempAllocatorSize = 32 * 1024 * 1024; // 32MB
};

struct RenderConfig {
	String64 appName;
	uint32_t width = 1280;
	uint32_t height = 720;
	bool vsync = true;
	bool enableValidation = true;
};

struct EngineConfig {
	PhysicsConfig physics;
	RenderConfig render;
};

} // namespace ZHLN
