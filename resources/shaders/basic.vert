#version 450

layout(push_constant) uniform PerObject { 
    mat4 transform; 
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;

void main() {
    // We will multiply ViewProj * Model on the CPU and push it
    gl_Position = transform * vec4(position, 1.0);
    vColor = color;
}