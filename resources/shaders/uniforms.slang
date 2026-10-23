module uniforms;

public enum LightType : uint {
    Directional = 0,
    Point = 1,
    Spot = 2,
    Area = 3,
    Sun = 4,
};

// TLAS instance-mask bits (RenderFrame.cpp assigns them per draw). The main
// world and Source's background layer (3D-skybox scenery, DrawFlags::
// Background) share one TLAS so each effect can choose what it sees: world
// first, layer on miss is the two-world rule, and shadow rays see the world
// only -- a skybox that could shadow the map is a skybox Source never had.
public static const uint kRayMaskSkyboxLayer = 0x01u;
public static const uint kRayMaskWorld       = 0x02u;

// Shared by the particle emitter ABI and its render push constants.
public enum ParticleAlignment : uint {
    CameraBillboard = 0,
    VelocityStretched = 1,
    GroundFlat = 2,
};

public struct FrameUniforms {
    public float4x4 viewProj;
    public float4x4 unjitteredViewProj;
    public float4x4 prevUnjitteredViewProj;

    public float4x4 lightSpaceMatrices[4];

    public float4x4 invViewProj;
    public float4 camPos;
    public float4 lightDir;
    public float4 sunRadiance; // RGB directional irradiance, after authored/cooked policy
    public uint lightCount;
    public float ambientExposure;
    public float shadowWidth;
    public uint shadowResolution;
    public float4 sh[9];
    public float4 probeMin;
    public float4 probeMax;
    public float4 probePos;
    public float4 jitterParams;
    public int enableRTR;
    public float sunSize;

    public float4 cascadeSplits;
    public int numCascades;
    public int fullBright;
    public float2 screenResolution;

    public float4 skyZenith;
    public float4 skyHorizon;
    public float4 skyGround;

    public float4x4 viewmodelViewProj;
    public float4x4 invProj;
    public float nearZ;
    public float farZ;
    public int environmentMode;
    public uint frameIndex;

    public float4 ddgiOriginSpacing;
    public float4 ddgiGridCounts;
    public float4 ddgiParams;
};

public struct GISettings {
    public int mode;
    public float aoRadius;
    public float aoBias;
    public float aoPower;
    public float giIntensity;
    public int giSamples;
    public float vignetteIntensity;
    public float vignettePower;
    public int enableSSR;
    public int enableRTR;
};

public struct AAState {
    public uint mode;
    public float taaFeedback;
    public float jitterX;
    public float jitterY;
    public float prevJitterX;
    public float prevJitterY;
    public uint frameIndex;
    public float fxaaSubpix;
    public float fxaaEdgeThreshold;
    public float fxaaEdgeThresholdMin;
    public float mlaaThreshold;
    public uint mlaaMaxSearchSteps;
};

// One rectangular tile of the shadow atlas: the light-space transform the tile
// was rasterized with, plus where it landed. A spot light owns one tile, a
// point light six (its cube faces, in Vulkan layer order).
//
// The projection is baked into `viewProj` rather than reconstructed here, so
// the lighting pass and the rasterizer cannot disagree about the depth range:
// both read the matrix the atlas allocator wrote this frame.
public struct ShadowTile {
    public float4x4 viewProj;
    public float2 uvOrigin; // tile origin in atlas UV
    public float uvScale;   // tile edge in atlas UV
    public float _pad;
};

public struct Light {
    public float3 position;
    public LightType type;
    public float3 color;
    public float intensity;
    public float3 direction;
    public float range;
    public float4 points[4];
    public float radius;
    public float innerConeCos;
    public float outerConeCos;
    public uint twoSided;
    // Who owns this light's shading where a map compile baked it into the
    // lightmaps (SceneLight::baked):
    //   0 - a live light; nothing is withheld,
    //   1 - an authored light entity: the bake owns its diffuse, its specular
    //       stays live because no lightmap stores one,
    //   2 - an emissive-surface proxy (a lamp panel/screen/sign the importer
    //       turned into a point light): the bake owns both, because the proxy
    //       itself is a reconstruction of light VRAD already integrated.
    // The punctual loop reads it against Res_BakedMask bit 1 in lighting.slang.
    public uint baked;
    // Index of this light's first ShadowTile, or -1 when it casts no shadow.
    // The renderer owns it: the atlas allocator assigns it each frame from the
    // lights that asked to cast and the tiles that still fit.
    public int shadowTileBase;
    public float4 positionView;
};
