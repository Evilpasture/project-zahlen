// resources/shaders/particle_update.hlsl
#pragma pack_matrix(column_major)
#include "uniforms.hlsl"

struct Particle {
    float3 position;
    float  life;
    float3 velocity;
    float  maxLife;
    float4 color;
    float  size;
    float3 _pad;
};

struct ParticlePushConstants {
    uint64_t particleBufferAddr;
    uint32_t particleCount;
    float    deltaTime;
};
[[vk::push_constant]] ParticlePushConstants pc;

[[vk::binding(2, 0)]] ConstantBuffer<FrameUniforms> frame;

// Fast GPU 3D Hash
float Hash13(float3 p) {
    p = frac(p * 0.1031f);
    p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}

float3 Hash33(float3 p) {
    return float3(Hash13(p), Hash13(p + float3(17.1f, 9.3f, 27.5f)), Hash13(p + float3(31.4f, 45.2f, 11.8f)));
}

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint idx = tid.x;
    if (idx >= pc.particleCount)
        return;

    // Load particle directly from GPU Buffer Device Address (BDA)
    uint64_t addr = pc.particleBufferAddr + idx * sizeof(Particle);
    Particle p    = vk::RawBufferLoad<Particle>(addr, 4);

    float3 camPos  = frame.camPos.xyz;
    float  boxSize = 40.0f; // 40m bounding box attached to camera
    float  time    = frame.camPos.w;

    // Advance motion
    p.position += p.velocity * pc.deltaTime;
    p.life -= pc.deltaTime;

    // Blizzard wind velocity + turbulent swirling sine offset
    float3 baseWind = float3(7.5f, -2.5f, 3.8f);
    float3 swirl    = float3(sin(time * 3.2f + p.position.z * 0.15f), cos(time * 2.1f + p.position.x * 0.15f), sin(time * 4.0f + p.position.y * 0.15f)) * 2.0f;

    p.velocity = baseWind + swirl;

    // Toroidal Wrapping: If a snowflake leaves the camera box, wrap it to the opposite side
    float3 relPos  = p.position - camPos;
    bool   wrapped = false;

    if (relPos.x > boxSize * 0.5f) {
        p.position.x -= boxSize;
        wrapped = true;
    }
    if (relPos.x < -boxSize * 0.5f) {
        p.position.x += boxSize;
        wrapped = true;
    }
    if (relPos.y > boxSize * 0.5f) {
        p.position.y -= boxSize;
        wrapped = true;
    }
    if (relPos.y < -boxSize * 0.5f) {
        p.position.y += boxSize;
        wrapped = true;
    }
    if (relPos.z > boxSize * 0.5f) {
        p.position.z -= boxSize;
        wrapped = true;
    }
    if (relPos.z < -boxSize * 0.5f) {
        p.position.z += boxSize;
        wrapped = true;
    }

    // Respawn expired or wrapped particles
    if (p.life <= 0.0f || wrapped) {
        float3 seed = float3(idx, time, pc.deltaTime);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        if (!wrapped) {
            p.position = camPos + rand * (boxSize * 0.5f);
        }
        p.life    = 2.0f + Hash13(seed) * 3.0f;
        p.maxLife = p.life;
        p.size    = 0.03f + Hash13(rand * 3.0f) * 0.05f;
        p.color   = float4(0.92f, 0.96f, 1.0f, 0.85f);
    }

    // Store updated particle state back to VRAM
    vk::RawBufferStore<Particle>(addr, p, 4);
}
