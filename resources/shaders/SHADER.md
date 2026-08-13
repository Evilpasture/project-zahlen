# SHADER GUIDE

All shaders are Slang. The build passes `-matrix-layout-column-major` to
slangc; do NOT try to change matrix storage in source (no
`#pragma pack_matrix`). Matrix/vector math in source must match the C++
`JPH::Mat44` column-major layout exactly.

RIGHT HANDED COORDINATES, COLUMN MAJOR/VECTOR COLUMN, CCW ONLY.

Descriptor binding authority lives in the shaders: the C++ side reflects
the compiled SPIR-V (`SlangReflectedLayout`) instead of declaring static
layouts. Keep `GlobalSceneRegistry` member order stable in `common.slang` —
binding numbers follow declaration order, and `globalTextures[]` (runtime
array) must stay LAST.
