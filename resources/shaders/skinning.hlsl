// resources/shaders/skinning.hlsl
#pragma pack_matrix(column_major)

// Flattened structure layout matching C++ precisely without compiler padding
struct Vertex {
	float pos_x;
	float pos_y;
	float pos_z;
	uint normal;
	uint tangent;
	uint uv;
	uint color;
	uint joint0;
	uint joint1;
	float weight0;
	float weight1;
	float weight2;
	float weight3;
	uint pad0;
	uint pad1;
	uint pad2;
};

struct GPUJoint {
	float4 col0;
	float4 col1;
	float4 col2;
	float4 col3;
};

struct SkinningConstants {
	uint64_t inputVerticesAddr;
	uint64_t outputVerticesAddr;
	uint64_t jointsAddr;
	uint32_t vertexCount;
	uint32_t jointOffset;
};

[[vk::push_constant]] SkinningConstants pcs;

float4 UnpackNormal(uint packed) {
	float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
	float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
	return float4(x, y, z, w);
}

uint PackNormal(float3 n, float w) {
	uint xs = (uint)((n.x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint ys = (uint)((n.y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint zs = (uint)((n.z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
	uint ws = (uint)(w > 0 ? 3 : 0) & 0x3;
	return (ws << 30) | (zs << 20) | (ys << 10) | xs;
}

float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset,
					uint64_t jointsAddr) {
	uint64_t j0_addr = jointsAddr + (jointOffset + joints.x) * 64;
	uint64_t j1_addr = jointsAddr + (jointOffset + joints.y) * 64;
	uint64_t j2_addr = jointsAddr + (jointOffset + joints.z) * 64;
	uint64_t j3_addr = jointsAddr + (jointOffset + joints.w) * 64;

	GPUJoint j0 = vk::RawBufferLoad<GPUJoint>(j0_addr, 4);
	GPUJoint j1 = vk::RawBufferLoad<GPUJoint>(j1_addr, 4);
	GPUJoint j2 = vk::RawBufferLoad<GPUJoint>(j2_addr, 4);
	GPUJoint j3 = vk::RawBufferLoad<GPUJoint>(j3_addr, 4);

	float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z +
				  j0.col3 * position.w) *
					 weights.x +
				 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z +
				  j1.col3 * position.w) *
					 weights.y +
				 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z +
				  j2.col3 * position.w) *
					 weights.z +
				 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z +
				  j3.col3 * position.w) *
					 weights.w;
	return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset,
					 uint64_t jointsAddr) {
	uint64_t j0_addr = jointsAddr + (jointOffset + joints.x) * 64;
	uint64_t j1_addr = jointsAddr + (jointOffset + joints.y) * 64;
	uint64_t j2_addr = jointsAddr + (jointOffset + joints.z) * 64;
	uint64_t j3_addr = jointsAddr + (jointOffset + joints.w) * 64;

	GPUJoint j0 = vk::RawBufferLoad<GPUJoint>(j0_addr, 4);
	GPUJoint j1 = vk::RawBufferLoad<GPUJoint>(j1_addr, 4);
	GPUJoint j2 = vk::RawBufferLoad<GPUJoint>(j2_addr, 4);
	GPUJoint j3 = vk::RawBufferLoad<GPUJoint>(j3_addr, 4);

	float3 dir =
		(j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) *
			weights.x +
		(j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) *
			weights.y +
		(j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) *
			weights.z +
		(j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) *
			weights.w;
	return dir;
}

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= pcs.vertexCount)
		return;

	// Load original vertex using push constant BDA address
	uint64_t inputAddr = pcs.inputVerticesAddr + tid.x * 64;
	Vertex v = vk::RawBufferLoad<Vertex>(inputAddr, 4);

	float4 localPos = float4(v.pos_x, v.pos_y, v.pos_z, 1.0f);
	float4 normRaw = UnpackNormal(v.normal);
	float4 tangRaw = UnpackNormal(v.tangent);

	// Unpack 16-bit joint indices from flattened 32-bit registers
	uint4 joints = uint4(v.joint0 & 0xFFFF, v.joint0 >> 16, v.joint1 & 0xFFFF, v.joint1 >> 16);
	float4 weights = float4(v.weight0, v.weight1, v.weight2, v.weight3);

	// Apply joint matrices
	float4 skinnedPos = SkinPosition(localPos, joints, weights, pcs.jointOffset, pcs.jointsAddr);
	float3 skinnedNorm =
		normalize(SkinDirection(normRaw.xyz, joints, weights, pcs.jointOffset, pcs.jointsAddr));
	float3 skinnedTang =
		normalize(SkinDirection(tangRaw.xyz, joints, weights, pcs.jointOffset, pcs.jointsAddr));

	// Pack attributes back into the output vertex
	Vertex outV;
	outV.pos_x = skinnedPos.x;
	outV.pos_y = skinnedPos.y;
	outV.pos_z = skinnedPos.z;
	outV.normal = PackNormal(skinnedNorm, normRaw.w);
	outV.tangent = PackNormal(skinnedTang, tangRaw.w);
	outV.uv = v.uv;
	outV.color = v.color;
	outV.joint0 = v.joint0;
	outV.joint1 = v.joint1;
	outV.weight0 = v.weight0;
	outV.weight1 = v.weight1;
	outV.weight2 = v.weight2;
	outV.weight3 = v.weight3;
	outV.pad0 = v.pad0;
	outV.pad1 = v.pad1;
	outV.pad2 = v.pad2;

	// Write directly to output scratch buffer
	uint64_t outputAddr = pcs.outputVerticesAddr + tid.x * 64;
	vk::RawBufferStore<Vertex>(outputAddr, outV, 4);
}
