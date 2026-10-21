#version 450
layout(location = 0) in vec2 inCorner;  // −1..1 quad
layout(location = 0) out vec2 vUV;

layout(push_constant) uniform Push {
  mat4 mvp;
  vec4 sunDir;   // xyz unit, w = half-size (world)
  vec4 _pad;     // keep layout aligned with moon push (unused)
} pc;

void main() {
  vec3 n = normalize(pc.sunDir.xyz);
  vec3 up = abs(n.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
  vec3 t = normalize(cross(up, n));
  vec3 b = cross(n, t);
  float h = pc.sunDir.w;
  vec3 pos = n * 93.0 + t * (inCorner.x * h) + b * (inCorner.y * h);
  vUV = inCorner;
  gl_Position = pc.mvp * vec4(pos, 1.0);
}
