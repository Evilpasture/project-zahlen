// resources/shaders/taa.hlsl

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
	float jitterX;
	float jitterY;
	float prevJitterX;
	float prevJitterY;
};
[[vk::push_constant]] PushConstants pc;

// --- COLOR SPACE CONVERSIONS ---
// Decouples luminance from chrominance to prevent chromatic ghosting
float3 RGBToYCoCg(float3 rgb) {
	float Y = dot(rgb, float3(0.25f, 0.50f, 0.25f));
	float Co = dot(rgb, float3(0.50f, 0.00f, -0.50f));
	float Cg = dot(rgb, float3(-0.25f, 0.50f, -0.25f));
	return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg) {
	float Y = ycocg.x;
	float Co = ycocg.y;
	float Cg = ycocg.z;
	float r = Y + Co - Cg;
	float g = Y + Cg;
	float b = Y - Co - Cg;
	return float3(r, g, b);
}

// Clips the history color vector toward the neighborhood average in YCoCg space,
// preserving the hue direction and preventing chromatic sparkling/clamping artifacts.
float3 ClipHistoryYCoCg(float3 history, float3 minColor, float3 maxColor, float3 mean) {
	float3 d = history - mean;

	// Prevent division by zero
	float3 invD = 1.0f / max(abs(d), 0.0001f);

	// Find the intersection distances with the box faces
	float3 t0 = (minColor - mean) * invD;
	float3 t1 = (maxColor - mean) * invD;

	float3 tMax = max(t0, t1);
	float t = saturate(min(min(tMax.x, tMax.y), tMax.z));

	return mean + d * t;
}

// --- OPTIMIZED 5-TAP CATMULL-ROM FILTER ---
// Uses 5 bilinear taps to reconstruct a high-quality 4x4 cubic filter, preserving sharp details
// [1.1.1]
float4 SampleTextureCatmullRom(Texture2D<float4> tex, SamplerState s, float2 uv, float2 texelSize) {
	float2 samplePos = uv / texelSize;
	float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;
	float2 f = samplePos - texPos1;

	// Cubic weights
	float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
	float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
	float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
	float2 w3 = f * f * (-0.5f + 0.5f * f);

	float2 w12 = w1 + w2;
	float2 tc12 = texPos1 + w2 / w12;

	float2 tc0 = texPos1 - 1.0f;
	float2 tc3 = texPos1 + 2.0f;

	float2 uv0 = tc0 * texelSize;
	float2 uv12 = tc12 * texelSize;
	float2 uv3 = tc3 * texelSize;

	float4 color = 0.0f;
	color += tex.SampleLevel(s, float2(uv12.x, uv0.y), 0) * (w12.x * w0.y);
	color += tex.SampleLevel(s, float2(uv0.x, uv12.y), 0) * (w0.x * w12.y);
	color += tex.SampleLevel(s, float2(uv12.x, uv12.y), 0) * (w12.x * w12.y);
	color += tex.SampleLevel(s, float2(uv3.x, uv12.y), 0) * (w3.x * w12.y);
	color += tex.SampleLevel(s, float2(uv12.x, uv3.y), 0) * (w12.x * w3.y);

	float weight =
		(w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
	if (weight > 0.0f) {
		color /= weight;
	}
	return color;
}

float4 PSMain(VSOutput input) : SV_Target0 {
	uint w, h;
	texCurrent.GetDimensions(w, h);
	float2 texelSize = 1.0f / float2(w, h);

	float2 velocity = texVelocity.SampleLevel(smp, input.uv, 0).rg;

	float2 historyUV = input.uv - velocity;

	float2 currentJitter = float2(pc.jitterX, pc.jitterY);
	float2 prevJitter = float2(pc.prevJitterX, pc.prevJitterY);

	float2 jitterDelta = float2(currentJitter.x - prevJitter.x, currentJitter.y - prevJitter.y);

	// FIX: Change -= to +=
	historyUV += jitterDelta;
	float4 current = texCurrent.SampleLevel(smp, input.uv, 0);

	// Sanitize incoming NaN pixels so they don't infect the frame
	if (any(isnan(current))) {
		current = float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	if (any(historyUV < 0.0f) || any(historyUV > 1.0f) || any(isnan(historyUV))) {
		return current;
	}

	// 1. High-fidelity history reconstruct sampling
	float4 history = SampleTextureCatmullRom(texHistory, smp, historyUV, texelSize);
	if (any(isnan(history))) {
		history = current;
	}

	// 2. Convert current and history to perceptual YCoCg space
	float3 currentYCoCg = RGBToYCoCg(current.rgb);
	float3 historyYCoCg = RGBToYCoCg(history.rgb);

	// 3. Compute 3x3 neighborhood statistics in YCoCg space
	float3 m1 = 0.0f;
	float3 m2 = 0.0f;

	for (int x = -1; x <= 1; ++x) {
		for (int y = -1; y <= 1; ++y) {
			float4 c = texCurrent.SampleLevel(smp, input.uv + float2(x, y) * texelSize, 0);
			float3 cYCoCg = RGBToYCoCg(c.rgb);
			m1 += cYCoCg;
			m2 += cYCoCg * cYCoCg;
		}
	}

	float3 mean = m1 / 9.0f;
	float3 stddev = sqrt(max(0.0f, (m2 / 9.0f) - (mean * mean)));

	// If m2 overflowed into Infinity anyway, stddev will become NaN. Catch it.
	if (any(isnan(stddev))) {
		stddev = 0.0f;
	}

	// 4. Perform variance clamping in YCoCg space
	float3 minColor = mean - stddev;
	float3 maxColor = mean + stddev;
	historyYCoCg = ClipHistoryYCoCg(historyYCoCg, minColor, maxColor, mean);

	// 5. Blend and convert the final result back to RGB
	float3 blendedYCoCg = lerp(currentYCoCg, historyYCoCg, pc.feedback);
	float3 finalRGB = YCoCgToRGB(blendedYCoCg);

	// Sanitize output to prevent any negative underflow values from the conversion math
	finalRGB = max(finalRGB, 0.0f);

	return float4(finalRGB, current.a);
}
