#version 440

// One retained instance per segment. Expand in logical pixels so fractional
// widths work identically with OpenGL, Vulkan, Metal and D3D, and at any DPR.
layout(location = 0) in vec4 endpoints;
layout(std140, binding = 0) uniform ChartUniforms {
    mat4 mvp;
    vec4 color;
    vec4 stroke; // clip units per logical pixel (xy), half width, point mode
} u;
layout(location = 0) out vec4 vColor;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(0, -1), vec2(1, -1), vec2(0, 1),
                                   vec2(0, 1), vec2(1, -1), vec2(1, 1));
    vec2 corner = corners[gl_VertexIndex];
    vec4 a = u.mvp * vec4(endpoints.xy, 0, 1);
    vec4 b = u.mvp * vec4(endpoints.zw, 0, 1);
    vec2 direction = (b.xy - a.xy) / u.stroke.xy;
    float magnitude = length(direction);
    vec2 normal = magnitude > 0 ? vec2(-direction.y, direction.x) / magnitude : vec2(0);
    vec2 offset = u.stroke.w > 0 ? vec2(corner.x * 2 - 1, corner.y) : normal * corner.y;
    gl_Position = mix(a, b, corner.x);
    gl_Position.xy += offset * u.stroke.xy * u.stroke.z;
    vColor = u.color;
}
