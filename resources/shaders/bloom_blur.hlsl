// resources/shaders/bloom_blur.hlsl
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
	int horizontal;
	float texelSize;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;

// Standard Gaussian weights
static const float weights[5] = {0.2270270270f, 0.1945945946f, 0.1216216216f, 0.0540540541f,
								 0.0162162162f};

float4 PSMain(VSOutput input) : SV_Target0 {
	float2 texelOffset =
		pc.horizontal != 0 ? float2(pc.texelSize, 0.0f) : float2(0.0f, pc.texelSize);
	float3 result = texInput.SampleLevel(smp, input.uv, 0).rgb * weights[0];

	for (int i = 1; i < 5; ++i) {
		result += texInput.SampleLevel(smp, input.uv + texelOffset * (float)i, 0).rgb * weights[i];
		result += texInput.SampleLevel(smp, input.uv - texelOffset * (float)i, 0).rgb * weights[i];
	}

	return float4(result, 1.0f);
}
