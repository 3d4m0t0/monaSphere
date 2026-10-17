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
  // Soft disc in UV space (−1..1).
  float d = length(vUV);
  float a = 1.0 - smoothstep(0.72, 1.0, d);
  if (a <= 0.001) discard;
  outColor = vec4(srgb_to_linear(vColor) * a, a);
}
