import pbr_helpers;

// Use a matching depth, normal, and pixel center at silhouettes. Shared by
// the horizon search and the bilateral reconstruction of its half-res output.
public bool GtaoSurface(Texture2D<float> depthTex, uint2 cell, uint2 fullSize,
                        out uint2 pixel, out float depth) {
    pixel = cell * 2u;
    depth = 1.0f;
    [unroll]
    for (uint y = 0u; y < 2u; ++y) {
        [unroll]
        for (uint x = 0u; x < 2u; ++x) {
            uint2 p = cell * 2u + uint2(x, y);
            if (all(p < fullSize)) {
                float d = depthTex.Load(int3(p, 0));
                if (d < depth) {
                    depth = d;
                    pixel = p;
                }
            }
        }
    }
    return depth < 1.0f;
}
