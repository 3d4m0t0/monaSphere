#version 450
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vLight;
layout(location = 0) out vec4 outColor;

vec3 srgb_to_linear(vec3 c) {
  bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
  vec3 low = c / 12.92;
  vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

void main() {
  float d = length(vUV);
  if (d > 1.0) discard;
  // Soft limb.
  float edge = 1.0 - smoothstep(0.92, 1.0, d);
  // Sphere normal from disc UV.
  float z = sqrt(max(0.0, 1.0 - d * d));
  vec3 N = normalize(vec3(vUV.x, vUV.y, z));
  vec3 L = normalize(vLight);
  float ndl = clamp(dot(N, L), 0.0, 1.0);
  // Soft terminator + faint earthshine on the night side.
  float lit = smoothstep(0.02, 0.35, ndl);
  float earthshine = 0.04 * (1.0 - lit);
  vec3 day = vec3(0.92, 0.90, 0.82);
  vec3 night = vec3(0.08, 0.09, 0.12);
  vec3 c = mix(night, day, lit) + earthshine;
  // Gentle center brightening.
  c *= 0.85 + 0.20 * z;
  outColor = vec4(srgb_to_linear(c) * edge, edge);
}
