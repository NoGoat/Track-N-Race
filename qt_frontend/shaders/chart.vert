#version 440

layout(location = 0) in vec2 position;

layout(std140, binding = 0) uniform ChartUniforms {
    mat4 mvp;
    vec4 color;
} u;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_Position = u.mvp * vec4(position, 0.0, 1.0);
    vColor = u.color;
}
