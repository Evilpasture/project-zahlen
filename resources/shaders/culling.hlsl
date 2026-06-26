#define SKIP_BINDINGS
#include "common.hlsl"

struct DrawIndirectCommand {
	uint vertexCount;
	uint instanceCount;
	uint firstVertex;
	uint firstInstance;
};

struct CullingConstants {
	float4 planes[6];
	uint drawCount;
	uint padding[3];
};

[[vk::binding(0, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<DrawIndirectCommand> g_indirectCommands;
[[vk::push_constant]] CullingConstants cullConstants;

bool SphereVisible(float3 center, float radius) {
	// OPTIMIZATION: Early-exit on first failed plane test
	[unroll(6)] for (uint i = 0; i < 6; ++i) {
		float4 probe = float4(center, 1.0f);
		float dist = dot(probe, cullConstants.planes[i]);
		if (dist < -radius) {
			return false; // Early-exit when occluded
		}
	}
	return true;
}

[numthreads(64, 1, 1)] void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint index = dispatchThreadID.x;
	if (index >= cullConstants.drawCount)
		return;

	InstanceData inst = g_instances[index];

	// Extract center using the piped offset
	float3 center = mul(inst.world, float4(inst.localCenter, 1.0f)).xyz;

	// FIX: Do NOT multiply by scale here! inst.cullRadius is already scaled.
	uint visible = SphereVisible(center, inst.cullRadius) ? 1u : 0u;

	bool isIndexed = inst.indexCount > 0;
	DrawIndirectCommand cmd;
	cmd.vertexCount = isIndexed ? inst.indexCount : inst.vertexCount;
	cmd.instanceCount = visible;
	cmd.firstVertex = 0;
	cmd.firstInstance = index;

	g_indirectCommands[index] = cmd;
}
