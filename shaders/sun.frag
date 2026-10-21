#version 450
layout(location = 0) in vec2 vUV;
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

  // Photosphere occupies the inner UV disc; rest of the quad is soft halo.
  const float rDisc = 0.42;

  float t = clamp(d / rDisc, 0.0, 1.0);
  float limb = pow(max(0.0, 1.0 - t * t), 0.70);
  float disc = (1.0 - smoothstep(0.96, 1.0, t)) * (0.28 + 0.72 * limb);

  // Soft, vague brightening around the disc (gentle Gaussian-like falloff).
  float halo = exp(-2.8 * max(d - rDisc * 0.85, 0.0) / max(1.0 - rDisc * 0.85, 1e-3));
  halo *= 1.0 - smoothstep(0.90, 1.0, d);
  halo *= 0.35;

  float glow = max(disc, halo);
  if (glow < 0.003) discard;

  vec3 hot = vec3(1.0, 0.98, 0.90);
  vec3 warm = vec3(1.0, 0.78, 0.42);
  vec3 haze = vec3(1.0, 0.70, 0.38);
  vec3 c = mix(warm, hot, limb);
  c = mix(haze, c, smoothstep(0.0, 0.55, disc));
  c *= mix(0.45, 0.60 + 1.10 * limb, smoothstep(0.0, 0.4, disc));

  outColor = vec4(srgb_to_linear(c) * glow, glow);
}
