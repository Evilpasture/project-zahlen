
import common;
import gpu_buffer;
import particles;
import uniforms;

struct ComputePushConstants {
    uint64_t particleBufferAddr;
    uint particleCount;
    float deltaTime;
    ParticleEmitterParams p;
};
[vk::push_constant]
ComputePushConstants pc;


[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint idx = tid.x;
    if (idx >= pc.particleCount)
        return;
    GPUBuffer<Particle> particleList = GPUBuffer<Particle>(pc.particleBufferAddr);
    Particle part = particleList.load(idx);

    float dt = pc.deltaTime;

    float3 accel = pc.p.gravity + SampleTurbulence(part.position.xyz, pc.p.turbulence, pc.p.turbulenceFreq, scene.frame.camPos.w);
    part.velocity.xyz += accel * dt;
    part.velocity.xyz *= exp(-pc.p.drag * dt);
    part.position.xyz += part.velocity.xyz * dt;

    part.params.x += dt;
    part.params.w += pc.p.spinSpeed * dt;

    float lifetime = max(part.params.y, 0.0001f);
    float t        = LifetimeAlpha(part.params.x, lifetime);
    part.color     = lerp(pc.p.startColor, pc.p.endColor, t);
    part.params.z  = lerp(pc.p.startSize.x, pc.p.endSize.x, t);

    bool wrapped = false;
    if (pc.p.loopBoundary > 0.5f) {
        float3 relPos = part.position.xyz - pc.p.spawnOrigin;
        if (abs(relPos.x) > pc.p.spawnBoxExtent.x * 0.5f) {
            part.position.x -= sign(relPos.x) * pc.p.spawnBoxExtent.x;
            wrapped = true;
        }
        if (abs(relPos.y) > pc.p.spawnBoxExtent.y * 0.5f) {
            part.position.y -= sign(relPos.y) * pc.p.spawnBoxExtent.y;
            wrapped = true;
        }
        if (abs(relPos.z) > pc.p.spawnBoxExtent.z * 0.5f) {
            part.position.z -= sign(relPos.z) * pc.p.spawnBoxExtent.z;
            wrapped = true;
        }
    }

    if (part.params.x >= part.params.y || (wrapped && pc.p.loopBoundary <= 0.5f) || part.params.y <= 0.0f) {
        float3 seed = float3(idx, scene.frame.camPos.w, dt);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        if (!wrapped || part.params.y <= 0.0f) {
            if (pc.p.spawnRadius > 0.0f) {
                part.position.xyz = pc.p.spawnOrigin + normalize(rand) * (Hash13(seed) * pc.p.spawnRadius);
            } else {
                part.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
            }
        }

        part.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        part.params.y     = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 2.0f));
        part.params.x     = 0.0f;
        part.params.w     = Hash13(seed + 3.0f) * 6.2831853f;
        part.color        = pc.p.startColor;
        part.params.z     = pc.p.startSize.x;
    }

    particleList.store(idx, part);
}
