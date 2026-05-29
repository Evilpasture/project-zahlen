// resources/shaders/toon.hlsl
#pragma pack_matrix(column_major)

struct Light {
	float3 position;
	uint type; // 0=Dir, 1=Point, 2=Spot
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float innerConeCos;
	float outerConeCos;
	float2 padding;
};

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 prevViewProj;
	float4x4 lightSpaceMatrix;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float3 padding;
};

struct ObjectConstants {
	float4x4 world;			// 64
	float4x4 prevWorld;		// 64
	uint albedoIdx;			// 4
	uint normalIdx;			// 4
	uint pbrIdx;			// 4
	uint emissiveIdx;		// 4
	uint isShadowPass;		// 4
	float metallicFactor;	// 4
	float roughnessFactor;	// 4
	float alphaCutoff;		// 4
	uint alphaMode;			// 4
	uint jointOffset;		// 4
	uint isSkinned;			// 4
	uint padding;			// 4  (Explicit padding before 16-byte aligned float4)
	float4 baseColorFactor; // 16
};

struct InstanceData {
	float4x4 world;			// 64
	float4x4 prevWorld;		// 64
	uint albedoIdx;			// 4
	uint normalIdx;			// 4
	uint pbrIdx;			// 4
	uint emissiveIdx;		// 4
	uint vertexCount;		// 4
	float cullRadius;		// 4
	float metallicFactor;	// 4
	float roughnessFactor;	// 4
	float alphaCutoff;		// 4
	uint alphaMode;			// 4
	uint jointOffset;		// 4
	uint isSkinned;			// 4
	float4 baseColorFactor; // 16
};

[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

struct GPUJoint {
	float4 col0;
	float4 col1;
	float4 col2;
	float4 col3;
};
[[vk::binding(7, 0)]] StructuredBuffer<GPUJoint> g_joints;

struct VSInput {
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(1)]] float3 normal : NORMAL;
	[[vk::location(2)]] float4 tangent : TANGENT;
	[[vk::location(3)]] float2 uv : TEXCOORD;
	[[vk::location(4)]] float4 color : COLOR;
	[[vk::location(5)]] uint4 joints : JOINTS;	  
	[[vk::location(6)]] float4 weights : WEIGHTS; 
};

struct VSOutput {
	float4 pos : SV_Position;
	float4 currClip : TEXCOORD0;
	float4 prevClip : TEXCOORD1;
	float3 worldPos : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD2;
	float4 shadowPos : TEXCOORD3;
	float4 color : COLOR;
	nointerpolation uint4 materialIndices : TEXCOORD4;
	nointerpolation float4 baseColorFactor : TEXCOORD5;
	nointerpolation float3 pbrFactors : TEXCOORD6; // x=metallic, y=roughness, z=alphaCutoff
	nointerpolation uint alphaMode : TEXCOORD7;
};

// --- SKELETAL SKINNING ---
float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z + j0.col3 * position.w) * weights.x +
				 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z + j1.col3 * position.w) * weights.y +
				 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z + j2.col3 * position.w) * weights.z +
				 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z + j3.col3 * position.w) * weights.w;
	return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset) {
	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float3 dir = (j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) * weights.x +
				 (j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) * weights.y +
				 (j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) * weights.z +
				 (j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) * weights.w;
	return dir;
}

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID) {
	VSOutput output;

	float4x4 worldMatrix;
	float4x4 prevWorldMatrix;
	uint albedoIdx, normalIdx, pbrIdx, emissiveIdx;
	float4 baseColorFactor;
	float metallicFactor, roughnessFactor, alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;

	if (obj.isShadowPass != 0 || obj.albedoIdx != 0) {
		// --- CPU TRADITIONAL PATH ---
		worldMatrix = obj.world;
		prevWorldMatrix = obj.prevWorld;
		albedoIdx = obj.albedoIdx;
		normalIdx = obj.normalIdx;
		pbrIdx = obj.pbrIdx;
		emissiveIdx = obj.emissiveIdx;
		baseColorFactor = obj.baseColorFactor;
		metallicFactor = obj.metallicFactor;
		roughnessFactor = obj.roughnessFactor;
		alphaCutoff = obj.alphaCutoff;
		alphaMode = obj.alphaMode;
		jointOffset = obj.jointOffset;
		isSkinned = obj.isSkinned;
	} else {
		// --- GPU CULLING PATH ---
		InstanceData inst = g_instances[instanceId];
		worldMatrix = inst.world;
		prevWorldMatrix = inst.prevWorld;
		albedoIdx = inst.albedoIdx;
		normalIdx = inst.normalIdx;
		pbrIdx = inst.pbrIdx;
		emissiveIdx = inst.emissiveIdx;
		baseColorFactor = inst.baseColorFactor;
		metallicFactor = inst.metallicFactor;
		roughnessFactor = inst.roughnessFactor;
		alphaCutoff = inst.alphaCutoff;
		alphaMode = inst.alphaMode;
		jointOffset = inst.jointOffset;
		isSkinned = inst.isSkinned;
	}

	float4 localPos = float4(input.position, 1.0f);
	float3 localNormal = input.normal * 2.0f - 1.0f;
	float3 localTangent = input.tangent.xyz * 2.0f - 1.0f;

	if (isSkinned != 0) {
		localPos = SkinPosition(localPos, input.joints, input.weights, jointOffset);
		localNormal = normalize(SkinDirection(localNormal, input.joints, input.weights, jointOffset));
		localTangent = normalize(SkinDirection(localTangent, input.joints, input.weights, jointOffset));
	}

	float4 worldPos = mul(worldMatrix, localPos);
	output.worldPos = worldPos.xyz;

	output.currClip = mul(frame.viewProj, worldPos);
	output.pos = output.currClip;

	float4 prevWorldPos = mul(prevWorldMatrix, localPos);
	output.prevClip = mul(frame.prevViewProj, prevWorldPos);

	float3x3 world3x3 = (float3x3)worldMatrix;

	output.normal = normalize(mul(world3x3, localNormal));
	output.tangent.xyz = normalize(mul(world3x3, localTangent));
	output.tangent.w = input.tangent.w;

	output.uv = input.uv;
	output.shadowPos = mul(frame.lightSpaceMatrix, worldPos);
	output.color = input.color;
	output.materialIndices = uint4(albedoIdx, normalIdx, pbrIdx, emissiveIdx);
	output.baseColorFactor = baseColorFactor;
	output.pbrFactors = float3(metallicFactor, roughnessFactor, alphaCutoff);
	output.alphaMode = alphaMode;

	return output;
}

// --- SHADOW CALCULATOR (Now declared before PSMain calls it) ---
float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;
	if (projCoords.z > 1.0 || projCoords.z < 0.0)
		return 1.0;
	float bias = max(0.005 * (1.0 - dot(N, L)), 0.001);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

struct PSOutput {
	float4 color : SV_Target0;
	float2 velocity : SV_Target1;
};

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;

	// Restore texture sampling combined with base color and vertex color attributes!
	float4 albedo = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	if (any(input.tangent.xyz)) {
		float3 T = normalize(input.tangent.xyz);
		float3 B = normalize(cross(N, T) * input.tangent.w);
		float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
		worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
	}

	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 L_sun = normalize(-frame.lightDir.xyz);

	// --- TOON LIGHTING RAMP ---
	float NdotL_sun = dot(worldNormal, L_sun);
	float halfLambert = NdotL_sun * 0.5f + 0.5f;

	float celIntensity;
	if (halfLambert < 0.25f) {
		celIntensity = 0.25f; // Hard shadow band
	} else if (halfLambert < 0.65f) {
		celIntensity = 0.65f; // Midtone band
	} else {
		celIntensity = 1.0f;  // Bright band
	}

	float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);
	celIntensity *= shadow;

	// --- TOON SPECULAR SHAPE ---
	float3 H_sun = normalize(V + L_sun);
	float NdotH_sun = max(dot(worldNormal, H_sun), 0.0f);
	float specular = step(0.98f, NdotH_sun) * 0.35f * shadow;

	// --- SHARP CONTOUR OUTLINE ---
	float rim = 1.0f - saturate(dot(worldNormal, V));
	float rimIntensity = smoothstep(0.6f, 0.75f, rim) * 0.25f * celIntensity;

	float3 ambient = albedo.rgb * 0.15f;
	float3 finalColor = ambient + (albedo.rgb * celIntensity) + specular + rimIntensity;

	output.color = float4(finalColor, albedo.a);

	float2 ndcCurr = input.currClip.xy / input.currClip.w;
	float2 ndcPrev = input.prevClip.xy / input.prevClip.w;
	output.velocity = (ndcCurr - ndcPrev) * 0.5f;

	return output;
}