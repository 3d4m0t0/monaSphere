#version 450
layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
  mat4 mvp;
  float gain_mul;
  uint _pad0;
  uint _pad1;
  uint _pad2;
} pc;

// Pre-baked milky way + nebulae + stars (equatorial / catalog space cubemap).
layout(set = 0, binding = 0) uniform samplerCube skyCube;

void main() {
  vec3 d = normalize(vDir);
  vec3 c = texture(skyCube, d).rgb;
  float gain = max(pc.gain_mul, 0.05);
  c *= gain;
  float a = max(max(c.r, c.g), c.b);
  outColor = vec4(c, a);
}
