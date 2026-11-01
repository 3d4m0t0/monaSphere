#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D skyMono;

// Tan-space FOV: x=left, y=right, z=up, w=down (OpenXR angles via tan).
layout(push_constant) uniform Push {
  vec4 eye_tan;
  vec4 mono_tan;
} pc;

void main() {
  // vUV.y=0 is top of the Vulkan framebuffer (= OpenXR angleUp).
  float tx = mix(pc.eye_tan.x, pc.eye_tan.y, vUV.x);
  float ty = mix(pc.eye_tan.z, pc.eye_tan.w, vUV.y);
  float denom_x = pc.mono_tan.y - pc.mono_tan.x;
  float denom_y = pc.mono_tan.z - pc.mono_tan.w;
  float u = (tx - pc.mono_tan.x) / max(denom_x, 1e-6);
  float v = (pc.mono_tan.z - ty) / max(denom_y, 1e-6);
  if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
    outColor = vec4(0.0);
    return;
  }
  outColor = texture(skyMono, vec2(u, v));
}
