// resources/shaders/postprocess_template.hlsl
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

struct PPPushConstants {
	float4x4 invViewProj;
	float4x4 viewProj;
	float4 camPos;
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int enableSSR;
	int pad;
};
[[vk::push_constant]] PPPushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> texColor;
[[vk::binding(1, 0)]] SamplerState linearSampler;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness;
[[vk::binding(4, 0)]] SamplerState pointSampler;
[[vk::binding(5, 0)]] TextureCube texEnvMap;

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 baseColor = texColor.SampleLevel(linearSampler, input.uv, 0).rgb;

	return float4(baseColor, 1.0f);
}
