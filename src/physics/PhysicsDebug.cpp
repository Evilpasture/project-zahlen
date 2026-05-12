#include "PhysicsDebug.hpp"

namespace ZHLN::Physics {

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo,
									JPH::ColorArg inColor) {
	uint32_t color = inColor.GetUInt32();
	lines.push_back({(float)inFrom.GetX(), (float)inFrom.GetY(), (float)inFrom.GetZ(), color});
	lines.push_back({(float)inTo.GetX(), (float)inTo.GetY(), (float)inTo.GetZ(), color});
}

void PhysicsDebugRenderer::DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3,
										JPH::ColorArg inColor, ECastShadow inCastShadow) {
	uint32_t color = inColor.GetUInt32();
	triangles.push_back({(float)inV1.GetX(), (float)inV1.GetY(), (float)inV1.GetZ(), color});
	triangles.push_back({(float)inV2.GetX(), (float)inV2.GetY(), (float)inV2.GetZ(), color});
	triangles.push_back({(float)inV3.GetX(), (float)inV3.GetY(), (float)inV3.GetZ(), color});
}

void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString,
									  JPH::ColorArg inColor, float inHeight) {
	// Usually ignored or printed to stdout in simple engines.
}

void PhysicsDebugRenderer::Clear() {
	lines.clear();
	triangles.clear();
}

} // namespace ZHLN::Physics