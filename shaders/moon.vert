#version 450
layout(location = 0) in vec2 inCorner;  // −1..1 quad
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vLight;   // sun in billboard tangent frame

layout(push_constant) uniform Push {
  mat4 mvp;
  vec4 moonDir;   // xyz unit, w = half-size (world)
  vec4 sunDir;    // xyz unit, w unused
} pc;

void main() {
  vec3 n = normalize(pc.moonDir.xyz);
  vec3 up = abs(n.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
  vec3 t = normalize(cross(up, n));
  vec3 b = cross(n, t);
  float h = pc.moonDir.w;
  vec3 pos = n * 92.0 + t * (inCorner.x * h) + b * (inCorner.y * h);
  vUV = inCorner;
  // Project sun into the moon disc's tangent plane for limb shading.
  vLight = vec3(dot(pc.sunDir.xyz, t), dot(pc.sunDir.xyz, b), dot(pc.sunDir.xyz, n));
  gl_Position = pc.mvp * vec4(pos, 1.0);
}
