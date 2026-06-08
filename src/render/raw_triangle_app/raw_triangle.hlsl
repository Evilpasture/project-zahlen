// src/render/raw_triangle_app/raw_triangle.hlsl

struct VSOutput {
	float4 pos : SV_Position;
	float3 color : COLOR;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;

	// Core coordinate offsets of our triangle
	float2 positions[3] = {float2(0.0f, -0.5f), float2(0.5f, 0.5f), float2(-0.5f, 0.5f)};

	float3 colors[3] = {
		float3(1.0f, 0.0f, 0.0f), // Red
		float3(0.0f, 1.0f, 0.0f), // Green
		float3(0.0f, 0.0f, 1.0f)  // Blue
	};

	output.pos = float4(positions[vertexID], 0.0f, 1.0f);
	output.color = colors[vertexID];
	return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
	return float4(input.color, 1.0f);
}
