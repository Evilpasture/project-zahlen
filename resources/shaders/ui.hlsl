// resources/shaders/ui.hlsl
#pragma pack_matrix(column_major)

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

// --- Custom Local Unpackers (Preventing Duplicate Definition Errors) ---
float2 LocalUnpackUV(uint packed) {
	return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

float4 LocalUnpackColor(uint packed) {
	return float4(float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f,
				  float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f);
}

struct UIObjectConstants {
	float4x4 orthoMatrix;
	uint64_t posAddress;
	uint64_t attrAddress;
	uint albedoIdx;
	uint padding;
};

[[vk::push_constant]] UIObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;

struct UIVSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

UIVSOutput VSMain(uint vertexId : SV_VertexID) {
	UIVSOutput output;

	float3 position = vk::RawBufferLoad<float3>(obj.posAddress + vertexId * 12, 4);
	uint uv = vk::RawBufferLoad<uint>(obj.attrAddress + vertexId * 16 + 8, 4);
	uint color = vk::RawBufferLoad<uint>(obj.attrAddress + vertexId * 16 + 12, 4);

	output.pos = mul(obj.orthoMatrix, float4(position, 1.0f));
	output.uv = LocalUnpackUV(uv);
	output.color = LocalUnpackColor(color);
	return output;
}

float4 PSMain(UIVSOutput input) : SV_Target0 {
	float4 textSample = globalTextures[obj.albedoIdx].Sample(defaultSampler, input.uv);
	return float4(input.color.rgb, input.color.a * textSample.a);
}
