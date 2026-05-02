#version 450
layout(std140, binding = 1) uniform Constants { mat4 transform; };
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = transform * vec4(position, 1.0);
    vColor = color;
}