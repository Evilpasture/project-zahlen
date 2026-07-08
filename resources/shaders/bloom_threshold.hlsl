// resources/shaders/bloom_threshold.hlsl
#pragma pack_matrix(column_major)

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
	return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 color = texInput.SampleLevel(smp, input.uv, 0).rgb;

	// 1. Calculate relative luminance (ITU-R BT.709)
	float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));

	// 2. Soft-Knee Thresholding
	// Replaces the harsh binary branch with a smooth quadratic curve transition
	float threshold = 1.0f;
	float knee = 0.5f; // Smoothness factor/width of the transition knee

	float soft = luma - threshold + knee;
	soft = clamp(soft, 0.0f, 2.0f * knee);
	soft = (soft * soft) / (4.0f * knee + 1e-5f);

	float contribution = max(soft, luma - threshold) / max(luma, 1e-5f);
	float3 brightColor = color * contribution;

	// 3. Highlight Energy Limiter (Anti-Warping Clamp)
	// Limits extreme firefly spikes (e.g. above 128.0) that overpower
	// downsampling, stabilizing the flare shape during camera movement
	float maxLuma = 128.0f;
	if (luma > maxLuma) {
		brightColor *= (maxLuma / luma);
	}

	return float4(brightColor, 1.0f);
}
