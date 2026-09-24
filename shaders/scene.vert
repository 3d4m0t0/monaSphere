#version 450
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec2 vUV;

layout(push_constant) uniform Push {
  mat4 mvp;
  int viewIndex;
  int stereoMode; // 0 mono 1 sbs 2 ou
  int is180;
  int _pad;
} pc;

void main() {
  vUV = inUV;
  gl_Position = pc.mvp * vec4(inPos, 1.0);
}
