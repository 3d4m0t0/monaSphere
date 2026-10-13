#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 0) out vec3 vColor;

layout(push_constant) uniform Push {
  mat4 mvp;
} pc;

void main() {
  vColor = inColor;
  gl_Position = pc.mvp * vec4(inPos, 1.0);
}
