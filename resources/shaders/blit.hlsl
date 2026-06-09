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
	// --- SSAO & SSGI CONFIGS (Aligned to 16 bytes) ---
	int giMode;		   // 0 = Off, 1 = SSAO, 2 = SSGI
	float aoRadius;	   // Sampling radius in meters
	float aoBias;	   // Depth bias to prevent surface acne self-occlusion
	float aoPower;	   // SSAO contrast multiplier
	float giIntensity; // SSGI bounce color multiplier
	int giSamples;	   // Hemisphere ray sample count
	float2 _pad;	   // Pad structure to perfect 16-byte bounds
};
[[vk::push_constant]] PushConstants pc;

// --- DITHERING & HEMISPHERE SAMPLERS ---

// 8 mathematically pre-calculated Poisson-distributed points on a unit hemisphere
// (Z is up, X/Y are tangent; guarantees an even spread with no clumping)
static const float3 HemisphereSamples[8] = {
	float3(0.35517f, -0.06385f, 0.93261f), float3(-0.19134f, 0.37512f, 0.90695f),
	float3(0.52841f, 0.50284f, 0.68412f),  float3(-0.67215f, -0.32111f, 0.66723f),
	float3(0.11211f, -0.78121f, 0.61432f), float3(-0.73211f, 0.42152f, 0.53512f),
	float3(0.81232f, -0.31211f, 0.49312f), float3(0.05211f, 0.88121f, 0.47012f)};

// High-performance Interleaved Gradient Noise (returns a single rotation angle [0, 1] per pixel)
float GetRotationAngle(float2 screenPos) {
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

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
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	// Skip background (infinite depth)
	if (depth >= 1.0f) {
		return float4(ACESFilm(litColor), 1.0f);
	}

	// 1. Retrieve world normal and material roughness
	float4 normRough = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = normalize(normRough.xyz * 2.0f - 1.0f);
	float roughness = normRough.w;

	// 2. Reconstruct World Position
	float3 worldPos = ReconstructWorldPos(input.uv, depth);

	// --- COOPERATIVE SSAO / SSGI / HBAO / GTAO EVALUATION PASS ---
	if (pc.giMode > 0) {
		float occlusion = 0.0f;
		float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

		// Screen-space edge vignette
		float2 edgeFactor = smoothstep(0.0f, 0.08f, input.uv) * smoothstep(1.0f, 0.92f, input.uv);
		float screenFade = edgeFactor.x * edgeFactor.y;

		// -------------------------------------------------------------
		// PATH A: GEOMETRIC HORIZON SEARCH (HBAO / GTAO)
		// -------------------------------------------------------------
		if (pc.giMode == 3 || pc.giMode == 4) {
			// 4 screen-space slice directions (0, 45, 90, 135 degrees)
			static const float2 sliceDirs[4] = {float2(1.0f, 0.0f), float2(0.7071f, 0.7071f),
												float2(0.0f, 1.0f), float2(-0.7071f, 0.7071f)};

			float angle = GetRotationAngle(input.pos.xy) * 2.0f * 3.14159265f;
			float cosTheta = cos(angle);
			float sinTheta = sin(angle);

			int steps = max(pc.giSamples / 4, 1);

			for (int d = 0; d < 4; ++d) {
				float2 rawDir = sliceDirs[d];
				float2 rotatedDir = float2(rawDir.x * cosTheta - rawDir.y * sinTheta,
										   rawDir.x * sinTheta + rawDir.y * cosTheta);

				// Step size decreases inversely with depth to maintain world-space consistency
				float2 uvStep = rotatedDir * (pc.aoRadius / max(depth, 0.01f)) * 0.05f;

				float max_sin_right = 0.0f;
				float max_sin_left = 0.0f;

				for (int i = 1; i <= steps; ++i) {
					float t = float(i) / float(steps);
					float2 uv_right = input.uv + uvStep * t;
					float2 uv_left = input.uv - uvStep * t;

					// 1. Trace Right (positive direction)
					if (all(uv_right >= 0.0f) && all(uv_right <= 1.0f)) {
						float d_right = texDepth.SampleLevel(smp, uv_right, 0).r;
						if (d_right < 1.0f) {
							float3 pos_right = ReconstructWorldPos(uv_right, d_right);
							float3 H = pos_right - worldPos;
							float len = length(H);
							if (len < pc.aoRadius) {
								float sin_elev = dot(H, N) / max(len, 0.001f);
								max_sin_right = max(max_sin_right, sin_elev);
							}
						}
					}

					// 2. Trace Left (negative direction - GTAO only)
					if (pc.giMode == 4 && all(uv_left >= 0.0f) && all(uv_left <= 1.0f)) {
						float d_left = texDepth.SampleLevel(smp, uv_left, 0).r;
						if (d_left < 1.0f) {
							float3 pos_left = ReconstructWorldPos(uv_left, d_left);
							float3 H = pos_left - worldPos;
							float len = length(H);
							if (len < pc.aoRadius) {
								float sin_elev = dot(H, N) / max(len, 0.001f);
								max_sin_left = max(max_sin_left, sin_elev);
							}
						}
					}
				}

				if (pc.giMode == 3) {
					// HBAO: accumulate single horizon angle
					occlusion += saturate(max_sin_right - pc.aoBias);
				} else if (pc.giMode == 4) {
					// GTAO: accumulate average of both horizon angles
					occlusion += saturate((max_sin_right + max_sin_left) * 0.5f - pc.aoBias);
				}
			}

			// Average the results over the 4 slice directions and apply contrast
			float ao = 1.0f - saturate((occlusion / 4.0f) * pc.aoPower * screenFade);
			litColor *= ao;
		}
		// -------------------------------------------------------------
		// PATH B: STOCHASTIC SPHERE SAMPLING (SSAO / SSGI)
		// -------------------------------------------------------------
		else {
			float angle = GetRotationAngle(input.pos.xy);
			float cosTheta = cos(angle * 2.0f * 3.14159265f);
			float sinTheta = sin(angle * 2.0f * 3.14159265f);

			for (int s = 0; s < pc.giSamples; ++s) {
				float3 sampleOffset = HemisphereSamples[s % 8];

				float3 rotatedSample =
					float3(sampleOffset.x * cosTheta - sampleOffset.y * sinTheta,
						   sampleOffset.x * sinTheta + sampleOffset.y * cosTheta, sampleOffset.z);

				float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
				float3 tangent = normalize(cross(up, N));
				float3 bitangent = cross(N, tangent);
				float3 dir =
					rotatedSample.x * tangent + rotatedSample.y * bitangent + rotatedSample.z * N;

				float3 testPos = worldPos + dir * pc.aoRadius;

				float4 clipPos = mul(pc.viewProj, float4(testPos, 1.0f));
				float3 ndc = clipPos.xyz / clipPos.w;
				float2 uv_sample = ndc.xy * float2(0.5f, -0.5f) + 0.5f;

				if (any(uv_sample < 0.0f) || any(uv_sample > 1.0f)) {
					continue;
				}

				float sampleRawDepth = texDepth.SampleLevel(smp, uv_sample, 0).r;
				if (sampleRawDepth >= 1.0f) {
					continue;
				}

				float3 sampleWorldPos = ReconstructWorldPos(uv_sample, sampleRawDepth);

				float rayDepthWorld = length(testPos - pc.camPos.xyz);
				float sampledDepthWorld = length(sampleWorldPos - pc.camPos.xyz);

				float diff = rayDepthWorld - sampledDepthWorld;

				if (diff > 0.0f) {
					float occlusionWeight = smoothstep(0.0f, pc.aoBias * 2.0f, diff - pc.aoBias);
					float rangeCheck = smoothstep(0.0f, 1.0f, pc.aoRadius / abs(diff));
					float finalWeight = occlusionWeight * rangeCheck;

					if (pc.giMode == 1) {
						occlusion += finalWeight;
					} else if (pc.giMode == 2) {
						float3 sampleColor = texInput.SampleLevel(smp, uv_sample, 0).rgb;
						float weight = max(dot(N, dir), 0.0f);
						indirectLight += sampleColor * weight * finalWeight;
					}
				}
			}

			if (pc.giMode == 1) {
				float ao =
					1.0f - saturate((occlusion / float(pc.giSamples)) * pc.aoPower * screenFade);
				litColor *= ao;
			} else if (pc.giMode == 2) {
				float3 indirect =
					(indirectLight / float(pc.giSamples)) * pc.giIntensity * screenFade;
				litColor += indirect;
			}
		}
	}

	// Skip raymarching entirely for fully rough surfaces (saves bandwidth)
	if (roughness > 0.85f) {
		return float4(ACESFilm(litColor), 1.0f);
	}

	// 3. Cast reflection ray (SSR)
	float3 V = normalize(worldPos - pc.camPos.xyz);
	float3 R = reflect(V, N);

	// 4. Execute Raymarch
	float confidence = 0.0f;
	float2 hitUV = RaymarchSSR(worldPos, R, confidence);

	// 5. Resolve SSR and blend
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

	// 6. Tonemap
	litColor = ACESFilm(litColor);
	return float4(litColor, 1.0f);
}
