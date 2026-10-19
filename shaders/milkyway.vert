#version 450
layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 vDir;

layout(push_constant) uniform Push {
  mat4 mvp;
  float gain_mul;
  uint _pad0;
  uint _pad1;
  uint _pad2;
} pc;

void main() {
  vDir = normalize(inPos);
  gl_Position = pc.mvp * vec4(inPos, 1.0);
}
