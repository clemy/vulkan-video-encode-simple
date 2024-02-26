#version 450

layout(location = 0) out vec3 fragColor;
layout(push_constant) uniform constants {
    uint currentFrameNumber;
} pushConstants;

vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

const float PI = 3.1415926535897932384626433832795;

void main() {
    const float speed = PI / 60;
    float offset = sin(float(pushConstants.currentFrameNumber) * speed);
    gl_Position = vec4(positions[gl_VertexIndex].x + offset, positions[gl_VertexIndex].y, 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}