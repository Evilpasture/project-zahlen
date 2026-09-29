
import common;
import math.transform;
import particles;
import uniforms;

struct ComputePushConstants {
    uint64_t particleBufferAddr;
    uint particleCount;
    float deltaTime;
    MeshParticleEmitterParams p;
};

[vk::push_constant]
ComputePushConstants pc;

[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint idx = tid.x;
    if (idx >= pc.particleCount)
        return;

    Particle3D* particleList = (Particle3D*) pc.particleBufferAddr;
    Particle3D p             = particleList[idx];
    float dt              = pc.deltaTime;

    if (p.params.y <= 0.0f) {
        float3 seed = float3(idx, scene.frame.camPos.w, pc.p.scaleMin);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        p.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
        p.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        p.rotVel.xyz   = lerp(pc.p.rotVelMin, pc.p.rotVelMax, Hash33(seed + 2.0f));
        p.rotation     = float4(0, 0, 0, 1);

        p.params.y   = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 3.0f));
        p.params.x   = 0.0f;
        p.position.w = pc.p.scaleMin;
        p.color      = pc.p.startColor;
    }
    else if (p.params.x < p.params.y) {
        p.params.x += dt;

        p.velocity.xyz += pc.p.gravity * dt;
        p.velocity.xyz *= exp(-pc.p.drag * dt);
        p.position.xyz += p.velocity.xyz * dt;

        float angle = length(p.rotVel.xyz) * dt;
        if (angle > 0.0001f) {
            Quaternion dq = Quaternion.fromAxisAngle(normalize(p.rotVel.xyz), angle);
            p.rotation    = Quaternion.multiply(dq, Quaternion(p.rotation)).normalized().value;
        }

        float t      = saturate(p.params.x / p.params.y);
        p.color      = lerp(pc.p.startColor, pc.p.endColor, t);
        p.position.w = lerp(pc.p.scaleMin, pc.p.scaleMax, t);
    }
    else if (pc.p.loopBoundary > 0.5f) {
        float3 seed = float3(idx, scene.frame.camPos.w, p.params.x);
        float3 rand = Hash33(seed) * 2.0f - 1.0f;

        p.position.xyz = pc.p.spawnOrigin + rand * (pc.p.spawnBoxExtent * 0.5f);
        p.velocity.xyz = lerp(pc.p.initVelMin, pc.p.initVelMax, Hash33(seed + 1.0f));
        p.rotVel.xyz   = lerp(pc.p.rotVelMin, pc.p.rotVelMax, Hash33(seed + 2.0f));
        p.rotation     = float4(0, 0, 0, 1);

        p.params.y   = lerp(pc.p.lifetimeMin, pc.p.lifetimeMax, Hash13(seed + 3.0f));
        p.params.x   = 0.0f;
        p.position.w = pc.p.scaleMin;
        p.color      = pc.p.startColor;
    }

    particleList[idx] = p;
}
