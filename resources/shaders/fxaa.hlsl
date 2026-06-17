// resources/shaders/fxaa.hlsl
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

struct PushConstants {
	float rcpFrameX;
	float rcpFrameY;
	float subpix;
	float edgeThreshold;
	float edgeThresholdMin;
	float _pad;
};
[[vk::push_constant]] PushConstants pc;

// Convert RGB to Luma (Y) for edge detection, incorporating a soft perceptual
// Reinhard tonemap curve locally so HDR sun glare doesn't smear the edge tracking.
float GetLuma(float3 rgb) {
	float luma = dot(rgb, float3(0.299, 0.587, 0.114));
	return luma / (1.0 + luma);
}

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 rgbM = texInput.SampleLevel(smp, input.uv, 0).rgb;

	float lumaM = GetLuma(rgbM);
	float lumaN = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(0, -1)).rgb);
	float lumaS = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(0, 1)).rgb);
	float lumaE = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(1, 0)).rgb);
	float lumaW = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(-1, 0)).rgb);

	float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
	float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
	float lumaRange = lumaMax - lumaMin;

	if (lumaRange < max(pc.edgeThresholdMin, lumaMax * pc.edgeThreshold)) {
		return float4(rgbM, 1.0f); // Bail out
	}

	float lumaNW = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(-1, -1)).rgb);
	float lumaNE = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(1, -1)).rgb);
	float lumaSW = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(-1, 1)).rgb);
	float lumaSE = GetLuma(texInput.SampleLevel(smp, input.uv, 0, int2(1, 1)).rgb);

	float lumaL = (lumaN + lumaS + lumaE + lumaW) * 2.0;
	float lumaCorners = lumaNW + lumaNE + lumaSW + lumaSE;
	float subpixFilter = saturate(abs((lumaL + lumaCorners) / 12.0 - lumaM) / lumaRange);
	float subpixBlend = smoothstep(0.0, 1.0, subpixFilter);
	subpixBlend = subpixBlend * subpixBlend * pc.subpix;

	float edgeHoriz = abs(-2.0 * lumaN + lumaNW + lumaNE) +
					  abs(-4.0 * lumaM + lumaW + lumaE) * 2.0 + abs(-2.0 * lumaS + lumaSW + lumaSE);
	float edgeVert = abs(-2.0 * lumaW + lumaNW + lumaSW) + abs(-4.0 * lumaM + lumaN + lumaS) * 2.0 +
					 abs(-2.0 * lumaE + lumaNE + lumaSE);
	bool isHorizontal = edgeHoriz >= edgeVert;

	float luma1 = isHorizontal ? lumaN : lumaW;
	float luma2 = isHorizontal ? lumaS : lumaE;
	float gradient1 = luma1 - lumaM;
	float gradient2 = luma2 - lumaM;

	bool is1Steepest = abs(gradient1) >= abs(gradient2);
	float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

	float stepLength = isHorizontal ? pc.rcpFrameY : pc.rcpFrameX;
	if (is1Steepest)
		stepLength = -stepLength;

	float lumaLocalAverage = 0.0;
	if (is1Steepest) {
		lumaLocalAverage = 0.5 * (luma1 + lumaM);
	} else {
		lumaLocalAverage = 0.5 * (luma2 + lumaM);
	}

	float2 currentUV = input.uv;
	if (isHorizontal) {
		currentUV.y += stepLength * 0.5;
	} else {
		currentUV.x += stepLength * 0.5;
	}

	float2 offset = isHorizontal ? float2(pc.rcpFrameX, 0.0) : float2(0.0, pc.rcpFrameY);
	float2 uv1 = currentUV - offset;
	float2 uv2 = currentUV + offset;

	float lumaEnd1 = 0.0;
	float lumaEnd2 = 0.0;
	bool reached1 = false;
	bool reached2 = false;

	// Quality preset 12 - steps up to 8 texels
	[unroll] for (int i = 0; i < 8; ++i) {
		if (!reached1)
			lumaEnd1 = GetLuma(texInput.SampleLevel(smp, uv1, 0).rgb) - lumaLocalAverage;
		if (!reached2)
			lumaEnd2 = GetLuma(texInput.SampleLevel(smp, uv2, 0).rgb) - lumaLocalAverage;

		reached1 = abs(lumaEnd1) >= gradientScaled;
		reached2 = abs(lumaEnd2) >= gradientScaled;

		if (reached1 && reached2)
			break;

		if (!reached1)
			uv1 -= offset * (i + 1.0);
		if (!reached2)
			uv2 += offset * (i + 1.0);
	}

	float dist1 = isHorizontal ? (input.uv.x - uv1.x) : (input.uv.y - uv1.y);
	float dist2 = isHorizontal ? (uv2.x - input.uv.x) : (uv2.y - input.uv.y);

	bool isDirection1 = dist1 < dist2;
	float distFinal = min(dist1, dist2);

	float edgeThickness = dist1 + dist2;
	float pixelOffset = -distFinal / edgeThickness + 0.5;

	bool isLumaCenterSmaller = lumaM < lumaLocalAverage;
	bool isLeftCorrect = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
	float finalOffset = isLeftCorrect ? pixelOffset : 0.0;

	finalOffset = max(finalOffset, subpixBlend);

	float2 finalUV = input.uv;
	if (isHorizontal) {
		finalUV.y += finalOffset * stepLength;
	} else {
		finalUV.x += finalOffset * stepLength;
	}

	return float4(texInput.SampleLevel(smp, finalUV, 0).rgb, 1.0f);
}
