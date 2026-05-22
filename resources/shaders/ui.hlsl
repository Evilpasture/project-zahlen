#pragma pack_matrix(column_major)

struct UIObjectConstants {
	float4x4 orthoMatrix; // Passes C++ screen projection matrix
	float4x4 unused;
	uint albedoIdx; // Bindless font atlas index
	uint padding[3];
};

[[vk::push_constant]] UIObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;

struct VSInput {
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(3)]] float2 uv : TEXCOORD;
	[[vk::location(4)]] float4 color : COLOR;
};

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

VSOutput VSMain(VSInput input) {
	VSOutput output;
	// Map pixels directly to Vulkan Clip Space
	output.pos = mul(obj.orthoMatrix, float4(input.position, 1.0f));
	output.uv = input.uv;
	output.color = input.color;
	return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
	// Sample texture atlas (Red channel serves as monochrome alpha mask)
	float4 textSample = globalTextures[obj.albedoIdx].Sample(defaultSampler, input.uv);

	// Discard zero-alpha parts to avoid drawing bounding boxes
	if (textSample.a < 0.1)
		discard;

	return float4(input.color.rgb, input.color.a * textSample.a);
}
