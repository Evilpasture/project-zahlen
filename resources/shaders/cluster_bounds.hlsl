#pragma pack_matrix(column_major)

struct ClusterBounds {
	float4 minPoint;
	float4 maxPoint;
};

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 unjitteredViewProj;
	float4x4 prevUnjitteredViewProj;
	float4x4 lightSpaceMatrix;
	float4x4 invViewProj;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float pad0;
	float pad1;
	float pad2;
	float4 sh[9];
	float4 probeMin;
	float4 probeMax;
	float4 probePos;
	float4 jitterParams;
	int enableRTR;
	float zScale;
	float zBias;
	int rtr_pad0;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<ClusterBounds> out_Bounds;
[[vk::binding(4,
			  0)]] ConstantBuffer<FrameUniforms> frame; // CHANGED FROM 5 TO 4 TO MATCH C++ LAYOUT

float4 Unproject(float4 coord) {
	float4 res = mul(frame.invViewProj, coord);
	return res / res.w;
}

[numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= 16 || tid.y >= 9)
		return;
	uint cIdx = tid.x + (tid.y * 16) + (tid.z * 144);

	float2 ts = float2(2.0f / 16.0f, 2.0f / 9.0f);
	float4 ndc[4] = {float4(-1.0f + tid.x * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f),
					 float4(-1.0f + tid.x * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f)};

	float3 pNear[4];
	float3 pFar[4];
	for (int i = 0; i < 4; ++i) {
		pNear[i] = Unproject(float4(ndc[i].xy, 0.0f, 1.0f)).xyz;
		pFar[i] = Unproject(float4(ndc[i].xy, 1.0f, 1.0f)).xyz;
	}

	float n = 0.1f;
	float f = 1000.0f;
	float sNear = n * pow(f / n, (float)tid.z / 24.0f);
	float sFar = n * pow(f / n, (float)(tid.z + 1) / 24.0f);

	float tNear = (sNear - n) / (f - n);
	float tFar = (sFar - n) / (f - n);

	float3 pMin = float3(1e30f, 1e30f, 1e30f);
	float3 pMax = float3(-1e30f, -1e30f, -1e30f);
	for (int j = 0; j < 4; ++j) {
		float3 ptNear = lerp(pNear[j], pFar[j], tNear);
		float3 ptFar = lerp(pNear[j], pFar[j], tFar);
		pMin = min(pMin, min(ptNear, ptFar));
		pMax = max(pMax, max(ptNear, ptFar));
	}

	out_Bounds[cIdx].minPoint = float4(pMin, 1.0f);
	out_Bounds[cIdx].maxPoint = float4(pMax, 1.0f);
}
