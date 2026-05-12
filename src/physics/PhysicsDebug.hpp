#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRendererSimple.h>

namespace ZHLN::Physics {

// 16-byte packed vertex ready for GPU upload
struct DebugVertex {
	float x, y, z;
	uint32_t color;
};

struct DebugDrawData {
	const DebugVertex* lines;
	size_t lineCount;
	const DebugVertex* triangles;
	size_t triangleCount;
};

class PhysicsDebugRenderer final : public JPH::DebugRendererSimple {
  public:
	PhysicsDebugRenderer() { Initialize(); }
	virtual ~PhysicsDebugRenderer() override = default;

	void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
	void DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3,
					  JPH::ColorArg inColor, ECastShadow inCastShadow) override;
	void DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString,
					JPH::ColorArg inColor, float inHeight) override;

	void Clear();

	JPH::Array<DebugVertex> lines;
	JPH::Array<DebugVertex> triangles;
};

} // namespace ZHLN::Physics