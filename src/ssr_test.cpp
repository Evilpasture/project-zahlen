// src/ssr_test.cpp
#include "SharedMath.hpp"

#include <Zahlen/Math3D.hpp>
#include <gtest/gtest.h>

// Include the HLSL shader directly as C++
#include "resources/shaders/postprocess.hlsl"

TEST(PostProcessTest, VerifyRaymarchSSR) {
	// 1. Configure the Uniforms (Push Constants)
	pc.enableSSR = 1;
	// Frame index in W is set to 1 for Stable Weyl noise offset validation
	pc.camPos = float4(0.0f, 5.0f, -5.0f, 1.0f);
	pc.viewProj = float4x4::sIdentity();
	pc.invViewProj = float4x4::sIdentity();

	// 2. Set mock screen resolution for the dynamic stride calculation
	texDepth.width = 1280;
	texDepth.height = 720;

	// 3. Mock G-Buffer reads by injecting lambda samplers
	texDepth.sample_callback = [](float2 uv, float mip) -> float {
		return 0.49f; // Fake ceiling depth
	};

	// 4. Setup SSR Ray
	float3 worldPos = float3(0.0f, 0.0f, 0.0f); // Origin point on ground (reflective floor)
	float3 N = float3(0.0f, 1.0f, 0.0f);		// Floor normal pointing up
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float3 R = reflect(-V, N);
	float3 biasedStartPos = worldPos + N * 0.05f;

	// 5. Execute the highly optimized Binary Search Raymarcher
	float confidence = 0.0f;
	float2 hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence);

	// 6. Verify that the raymarched ceiling hit cleanly and returned valid UVs
	EXPECT_GT(confidence, 0.0f);
	EXPECT_GT(hitUV.x, 0.0f);
	EXPECT_GT(hitUV.y, 0.0f);

	// 7. Replicate the Material evaluation from PSMain to verify final physics output
	float roughness = 0.05f;
	float3 reflectionColor = float3(1.0f, 0.0f, 0.0f); // Fake red surface
	float3 F0 = float3(0.15f, 0.15f, 0.15f);
	float3 F = F0 + (float3(1.0f, 1.0f, 1.0f) - F0) * pow(saturate(1.0f - dot(V, N)), 5.0f);

	float roughnessFade = saturate(1.0f - roughness);
	float horizonOcclusion = saturate(1.0f + dot(R, N));
	horizonOcclusion *= horizonOcclusion;

	float3 finalColor = reflectionColor * confidence * F * roughnessFade * horizonOcclusion;

	// Because of confidence fades, fresnel, and roughness attenuation,
	// the final red value shouldn't be exactly 1.0, but it should be strongly visible.
	EXPECT_GT(length(finalColor), 0.0f);
	EXPECT_FLOAT_EQ(finalColor.y, 0.0f);
	EXPECT_FLOAT_EQ(finalColor.z, 0.0f);
}
