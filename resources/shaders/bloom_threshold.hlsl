// resources/shaders/bloom_threshold.hlsl
#pragma pack_matrix(column_major)

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);
	return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 color = texInput.SampleLevel(smp, input.uv, 0).rgb;

	// Calculate relative luminance (ITU-R BT.709)
	float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

	// Isolate pixels above threshold (e.g., 1.0)
	float threshold = 1.0f;
	float3 brightColor = float3(0.0f, 0.0f, 0.0f);
	if (luma > threshold) {
		brightColor = color;
	}

	return float4(brightColor, 1.0f);
}
