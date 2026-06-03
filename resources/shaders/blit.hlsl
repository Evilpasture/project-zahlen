struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// Negate the Y coordinate to compensate for the negative viewport height
	output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);

	return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 color = texInput.SampleLevel(smp, input.uv, 0).rgb;

	// Reinhard ACES Tonemap
	color = color / (color + 1.0f);

	// Display Gamma Correction
	// color = pow(color, 1.0f / 2.2f);

	return float4(color, 1.0f);
}
