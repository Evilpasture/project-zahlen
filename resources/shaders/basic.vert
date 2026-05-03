#version 450

layout(std140, binding = 1) uniform GlobalData { 
    mat4 viewProj; 
};

// This block name doesn't matter, but the internal member name 
// should ideally match your C++ UniformDescriptor name for some backends.
layout(push_constant) uniform PerObject { 
    mat4 model; 
};

layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = viewProj * model * vec4(position, 1.0);
    vColor = color;
}