struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

// Generates a fullscreen triangle without vertex buffers
VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// Negate the Y coordinate to compensate for the negative viewport height
	output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);

	return output;
}

[[vk::binding(0, 0)]] Texture2D<float4> texCurrent;
[[vk::binding(1, 0)]] Texture2D<float4> texHistory;
[[vk::binding(2, 0)]] Texture2D<float2> texVelocity;
[[vk::binding(3, 0)]] SamplerState smp;

struct PushConstants {
	float feedback;
};
[[vk::push_constant]] PushConstants pc;

float4 PSMain(VSOutput input) : SV_Target0 {
	float2 velocity = texVelocity.SampleLevel(smp, input.uv, 0).rg;
	float2 historyUV = input.uv - velocity;

	float4 current = texCurrent.SampleLevel(smp, input.uv, 0);

	// FIX: Sanitize incoming NaN pixels so they don't infect the frame
	if (any(isnan(current))) {
		current = float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	if (any(historyUV < 0.0f) || any(historyUV > 1.0f) || any(isnan(historyUV))) {
		return current;
	}

	float4 history = texHistory.SampleLevel(smp, historyUV, 0);
	if (any(isnan(history))) {
		history = current;
	}

	float4 m1 = 0;
	float4 m2 = 0;

	uint w, h;
	texCurrent.GetDimensions(w, h);
	float2 texelSize = 1.0f / float2(w, h);

	for (int x = -1; x <= 1; ++x) {
		for (int y = -1; y <= 1; ++y) {
			float4 c = texCurrent.SampleLevel(smp, input.uv + float2(x, y) * texelSize, 0);
			m1 += c;
			m2 += c * c;
		}
	}

	float4 mean = m1 / 9.0f;
	float4 stddev = sqrt(max(0.0f, (m2 / 9.0f) - (mean * mean)));

	// FIX: If m2 overflowed into Infinity anyway, stddev will become NaN. Catch it.
	if (any(isnan(stddev))) {
		stddev = 0.0f;
	}

	history = clamp(history, mean - stddev, mean + stddev);

	return lerp(current, history, pc.feedback);
}
