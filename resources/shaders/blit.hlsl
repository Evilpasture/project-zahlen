// resources/shaders/blit.hlsl
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

struct PushConstants {
	float vignetteIntensity;
	float vignettePower;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float4> texBloom; // <-- Bound dynamically by BlitPass

float3 ACESFilm(float3 x) {
	float a = 2.51f;
	float b = 0.03f;
	float c = 2.43f;
	float d = 0.59f;
	float e = 0.14f;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 hdrColor = texInput.SampleLevel(smp, input.uv, 0).rgb;

	// Sample Bloom and blend additively (adjust the 0.5f intensity multiplier as needed)
	float3 bloom = texBloom.SampleLevel(smp, input.uv, 0).rgb;
	hdrColor += bloom * 0.5f;

	// 1. Exposure Control: Scale down the massive HDR values (Sun is 250.0)
	hdrColor *= 0.015f;

	// 2. Tonemap HDR -> LDR
	float3 finalColor = ACESFilm(hdrColor);

	// 3. Apply Vignette overlay
	if (pc.vignetteIntensity > 0.0f) {
		float2 uvDist = abs(input.uv - 0.5f) * pc.vignetteIntensity;
		float vignette = saturate(1.0f - dot(uvDist, uvDist));
		vignette = pow(vignette, max(pc.vignettePower, 0.01f));
		finalColor *= vignette;
	}

	return float4(finalColor, 1.0f);
}
