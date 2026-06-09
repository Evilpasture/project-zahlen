// resources/shaders/blit.hlsl
#pragma pack_matrix(column_major)

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

struct PushConstants {
	float4x4 invViewProj;
	float4x4 viewProj;
	float4 camPos;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> texInput;			// Forward Lit Color
[[vk::binding(1, 0)]] SamplerState smp;						// Sampler
[[vk::binding(2, 0)]] Texture2D<float> texDepth;			// Raw Depth Buffer
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness; // G-Buffer Normal/Roughness
[[vk::binding(4, 0)]] SamplerState pointSampler;			// Nearest Sampler

// Reconstruct 3D World Space Position from Clip Space Depth
float3 ReconstructWorldPos(float2 uv, float depth) {
	float4 clipSpacePos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
	float4 worldSpacePos = mul(pc.invViewProj, clipSpacePos);
	return worldSpacePos.xyz / worldSpacePos.w;
}

// Refines the intersection point down to sub-centimeter accuracy
float2 BinarySearch(float3 dir, float3 currentPos, out float confidence) {
	float3 step = dir * 0.25f;		  // Coarse step size (matches RaymarchSSR)
	float3 start = currentPos - step; // The position at the previous step (in front)
	float3 end = currentPos;		  // The position at the current step (behind)
	float3 mid = currentPos;
	float2 uv = float2(0, 0);

	// 5 iterations refine the hit point from 25cm down to ~0.7cm
	[unroll(5)] for (int i = 0; i < 5; ++i) {
		mid = (start + end) * 0.5f;

		// Project current midpoint to screen space
		float4 clipPos = mul(pc.viewProj, float4(mid, 1.0f));
		float3 ndc = clipPos.xyz / clipPos.w;
		uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;

		float sampledDepth = texDepth.SampleLevel(pointSampler, uv, 0).r;
		float3 sampledWorldPos = ReconstructWorldPos(uv, sampledDepth);

		float rayDepthWorld = length(mid - pc.camPos.xyz);
		float sampledDepthWorld = length(sampledWorldPos - pc.camPos.xyz);

		if (rayDepthWorld >= sampledDepthWorld) {
			end = mid; // Still behind, move end point closer
		} else {
			start = mid; // In front, move start point further
		}
	}

	// Final verification of the refined intersection
	float finalDepth = texDepth.SampleLevel(pointSampler, uv, 0).r;
	float3 finalWorldPos = ReconstructWorldPos(uv, finalDepth);
	float finalRayDepth = length(mid - pc.camPos.xyz);
	float finalSampledDepth = length(finalWorldPos - pc.camPos.xyz);

	float thicknessWorld = 0.3f; // Comfortable world space thickness allowance
	if (abs(finalRayDepth - finalSampledDepth) < thicknessWorld) {
		confidence = 1.0f;

		// RESTORED: Smoothly fade reflections out as they approach any screen edge
		float2 edgeFactor = smoothstep(0.0f, 0.08f, uv) * smoothstep(1.0f, 0.92f, uv);
		confidence *= edgeFactor.x * edgeFactor.y;
	} else {
		confidence = 0.0f;
	}

	return uv;
}

// Full Screen Linear Raymarching
float2 RaymarchSSR(float3 startPos, float3 dir, out float confidence) {
	float stepSize = 0.25f; // Step size in world units
	float3 currentPos = startPos + dir * stepSize;
	confidence = 0.0f;

	// Linear Coarse Raymarching Loop
	[unroll(40)] for (int i = 0; i < 40; ++i) {
		// Project current ray position back to screen space
		float4 clipPos = mul(pc.viewProj, float4(currentPos, 1.0f));
		float3 ndc = clipPos.xyz / clipPos.w;
		float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;

		// Skip if ray goes out of screen bounds
		if (any(uv < 0.0f) || any(uv > 1.0f)) {
			break;
		}

		float sampledDepth = texDepth.SampleLevel(pointSampler, uv, 0).r;

		// Skip background (infinite depth)
		if (sampledDepth >= 1.0f) {
			currentPos += dir * stepSize;
			continue;
		}

		float3 sampledWorldPos = ReconstructWorldPos(uv, sampledDepth);

		float rayDepthWorld = length(currentPos - pc.camPos.xyz);
		float sampledDepthWorld = length(sampledWorldPos - pc.camPos.xyz);

		// Trigger Binary Search the moment we pass behind a surface
		if (rayDepthWorld >= sampledDepthWorld) {
			return BinarySearch(dir, currentPos, confidence); // <--- UPDATE THIS
		}

		currentPos += dir * stepSize;
	}

	return float2(0.0f, 0.0f);
}
float3 ACESFilm(float3 x) {
	float a = 2.51f;
	float b = 0.03f;
	float c = 2.43f;
	float d = 0.59f;
	float e = 0.14f;
	return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 litColor = texInput.SampleLevel(smp, input.uv, 0).rgb;
	// USE THE POINT SAMPLER HERE AS WELL FOR ACCURATE DEPTH RECONSTRUCTION
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	// Skip background (infinite depth)
	if (depth >= 1.0f) {
		return float4(ACESFilm(litColor), 1.0f);
	}

	// 1. Retrieve world normal and material roughness
	float4 normRough = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = normalize(normRough.xyz * 2.0f - 1.0f);
	float roughness = normRough.w;

	// Skip raymarching entirely for fully rough surfaces (saves bandwidth)
	if (roughness > 0.85f) {
		return float4(ACESFilm(litColor), 1.0f);
	}

	// 2. Reconstruct World Position & cast reflection ray
	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float3 V = normalize(worldPos - pc.camPos.xyz);
	float3 R = reflect(V, N);

	// 3. Execute Raymarch
	float confidence = 0.0f;
	float2 hitUV = RaymarchSSR(worldPos, R, confidence);

	// 4. Resolve SSR and blend
	if (confidence > 0.0f) {
		float3 reflectionColor = texInput.SampleLevel(smp, hitUV, 0).rgb;

		// Apply simple Schlick Fresnel to the SSR contribution
		float3 F0 = float3(0.04, 0.04, 0.04); // Default dielectrics
		float3 F = F0 + (1.0 - F0) * pow(saturate(1.0 - dot(-V, N)), 5.0);

		// Fade reflection on rougher surfaces
		float roughnessFade = saturate(1.0f - roughness);
		float3 ssr = reflectionColor * F * confidence * roughnessFade;

		// Blend: damp original specular highlight on reflected regions
		litColor = lerp(litColor, litColor + ssr, roughnessFade * confidence);
	}

	// 5. Tonemap
	litColor = ACESFilm(litColor);
	return float4(litColor, 1.0f);
}
