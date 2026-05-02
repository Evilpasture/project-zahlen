#include <metal_stdlib>
using namespace metal;
struct Constants { float4x4 transform; };
struct VertexIn  { float3 position [[attribute(0)]]; float4 color [[attribute(1)]]; };
struct VertexOut { float4 position [[position]]; float4 color; };
vertex VertexOut vsMain(VertexIn in [[stage_in]], constant Constants& cBuffer [[buffer(1)]]) {
    VertexOut out; out.position = cBuffer.transform * float4(in.position, 1.0); out.color = in.color; return out;
}
fragment float4 fsMain(VertexOut in [[stage_in]]) { return in.color; }