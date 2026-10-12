
import common;
import uniforms;

struct PushConstants {
    uint lightIndex;
};
[vk::push_constant]
PushConstants pc;

struct PunctualShadowVSOutput {
    float4 pos : SV_Position;
    float linearDepth : TEXCOORD0;
};

PunctualShadowVSOutput VSMain(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID, uint viewId: SV_ViewID) {
    PunctualShadowVSOutput output;

    InstanceData inst   = scene.g_instances[instanceId];
    uint actualVertexId = inst.fetchRasterIndex(vertexId);
    actualVertexId      = min(actualVertexId, max(inst.vertexCount, 1u) - 1u);

    float3 localPos = inst.fetchPosition(actualVertexId);
    float3 worldPos = mul(inst.world, float4(localPos, 1.0f)).xyz;

    Light light     = scene.lights[pc.lightIndex];
    float3 lightPos = light.position;

    // The multiview layers are the six cubemap faces in Vulkan layer order
    // (+X,-X,+Y,-Y,+Z,-Z) and the lighting shader samples this atlas through
    // a TextureCubeArray, i.e. with the hardware's fixed face/UV convention.
    // The rasterized texels therefore have to land exactly where the spec's
    // major-axis table puts them: for a face with major component ma and
    // side components (sc, tc),
    //
    //   u = (sc/|ma| + 1) / 2     v = (tc/|ma| + 1) / 2
    //
    // Emitting clip = (sc, tc, 0, |ma|) lets the hardware's perspective
    // interpolation form those ratios per fragment, and triangles straddling
    // a face boundary clip naturally at the viewport edge. The previous
    // look-at + perspective projection matched neither the hardware's UV
    // table (its Y axis came out mirrored) nor its 90-degree FOV scale (xy
    // was amplified ~ (f-n)/(n*f)), so interior fragments compared their
    // depth against texels written for mirrored/squashed directions: rooms
    // went black while pools leaked onto exterior surfaces.
    float3 d  = worldPos - lightPos;
    float  dl = length(d);
    d = (dl > 1e-6f) ? d / dl : float3(0.0f, 0.0f, 1.0f);

    float sc, tc, ma;
    if (viewId == 0) {
        sc = -d.z; tc = -d.y; ma =  d.x;   // +X
    } else if (viewId == 1) {
        sc =  d.z; tc = -d.y; ma = -d.x;   // -X
    } else if (viewId == 2) {
        sc =  d.x; tc =  d.z; ma =  d.y;   // +Y
    } else if (viewId == 3) {
        sc =  d.x; tc = -d.z; ma = -d.y;   // -Y
    } else if (viewId == 4) {
        sc =  d.x; tc = -d.y; ma =  d.z;   // +Z
    } else {
        sc = -d.x; tc = -d.y; ma = -d.z;   // -Z
    }

    output.pos = float4(sc, tc, 0.0f, abs(ma));

    float f = max(light.range, 1.0f);
    output.linearDepth = dl / f;
    return output;
}

struct PunctualShadowPSOutput {
    float depth : SV_Depth;
};

PunctualShadowPSOutput PSMain(PunctualShadowVSOutput input) {
    PunctualShadowPSOutput output;
    output.depth = saturate(input.linearDepth);
    return output;
}
