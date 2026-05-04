#version 450

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 v_color;

const vec3 positions[24] = vec3[24](
    // Front (+Z)
    vec3(-1,-1, 1), vec3( 1,-1, 1), vec3( 1, 1, 1), vec3(-1, 1, 1),
    // Back (-Z)
    vec3( 1,-1,-1), vec3(-1,-1,-1), vec3(-1, 1,-1), vec3( 1, 1,-1),
    // Top (+Y)
    vec3(-1, 1, 1), vec3( 1, 1, 1), vec3( 1, 1,-1), vec3(-1, 1,-1),
    // Bottom (-Y)
    vec3(-1,-1,-1), vec3( 1,-1,-1), vec3( 1,-1, 1), vec3(-1,-1, 1),
    // Right (+X)
    vec3( 1,-1, 1), vec3( 1,-1,-1), vec3( 1, 1,-1), vec3( 1, 1, 1),
    // Left (-X)
    vec3(-1,-1,-1), vec3(-1,-1, 1), vec3(-1, 1, 1), vec3(-1, 1,-1)
);

const int indices[36] = int[36](
     0,  1,  2,  2,  3,  0,
     4,  5,  6,  6,  7,  4,
     8,  9, 10, 10, 11,  8,
    12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,
    20, 21, 22, 22, 23, 20
);

void main() {
    int index = indices[gl_VertexIndex];
    vec3 pos = positions[index];
    gl_Position = pc.mvp * vec4(pos, 1.0);
    v_color = normalize(pos) * 0.5 + 0.5;
}
