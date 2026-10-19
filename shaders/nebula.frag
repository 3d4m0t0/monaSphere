#version 450
layout(location = 0) in vec3 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Display sRGB → linear. The eye swapchain is sRGB.
vec3 srgb_to_linear(vec3 c) {
  bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
  vec3 low = c / 12.92;
  vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

void main() {
  float d = length(vUV);
  // Wide, heavily blurred shell + compact brighter core.
  float shell = exp(-0.85 * d * d);
  shell *= smoothstep(1.0, 0.25, d);
  float core = exp(-14.0 * d * d);
  float haze = shell * 0.70 + core * 1.10;
  if (haze <= 0.001) discard;

  // Desaturate the cloud tint; core keeps a bit more hue.
  vec3 c = vColor;
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float sat = mix(0.22, 0.48, clamp(core * 1.2, 0.0, 1.0));
  c = clamp(luma + (c - luma) * sat, 0.0, 1.0);

  float glow = shell * 0.055 + core * 0.26;
  outColor = vec4(srgb_to_linear(c) * glow, haze * 0.16);
}
