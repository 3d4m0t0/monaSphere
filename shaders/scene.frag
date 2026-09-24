#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D videoTex;   // RGBA or Y
layout(set = 0, binding = 1) uniform sampler2D videoTexUV; // UV (NV12) or unused

layout(push_constant) uniform Push {
  mat4 mvp;
  int viewIndex;
  int stereoMode;
  int is180;
  // 0 = RGBA, 1 = NV12 limited (TV) BT.709, 2 = NV12 full (PC) BT.709
  int nv12;
} pc;

vec3 yuv_to_rgb_limited(float y, float u, float v) {
  y = (y - 16.0 / 255.0) * (255.0 / 219.0);
  u = (u - 128.0 / 255.0) * (255.0 / 224.0);
  v = (v - 128.0 / 255.0) * (255.0 / 224.0);
  float r = y + 1.5748 * v;
  float g = y - 0.1873 * u - 0.4681 * v;
  float b = y + 1.8556 * u;
  return clamp(vec3(r, g, b), 0.0, 1.0);
}

vec3 yuv_to_rgb_full(float y, float u, float v) {
  u = u - 0.5;
  v = v - 0.5;
  float r = y + 1.5748 * v;
  float g = y - 0.1873 * u - 0.4681 * v;
  float b = y + 1.8556 * u;
  return clamp(vec3(r, g, b), 0.0, 1.0);
}

// Display-referred sRGB → linear (swapchain is VK_FORMAT_*_SRGB).
vec3 srgb_to_linear(vec3 c) {
  bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
  vec3 low = c / 12.92;
  vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

void main() {
  vec2 uv = vUV;
  if (pc.is180 != 0) {
    if (uv.x < 0.25 || uv.x > 0.75) {
      discard;
    }
    uv.x = (uv.x - 0.25) * 2.0;
  }

  if (pc.stereoMode == 1) {
    float off = (pc.viewIndex == 0) ? 0.0 : 0.5;
    uv.x = uv.x * 0.5 + off;
  } else if (pc.stereoMode == 2) {
    float off = (pc.viewIndex == 0) ? 0.0 : 0.5;
    uv.y = uv.y * 0.5 + off;
  }

  if (pc.nv12 == 1 || pc.nv12 == 2) {
    float y = texture(videoTex, uv).r;
    vec2 chroma = texture(videoTexUV, uv).rg;
    vec3 rgb = (pc.nv12 == 2) ? yuv_to_rgb_full(y, chroma.r, chroma.g)
                              : yuv_to_rgb_limited(y, chroma.r, chroma.g);
    // YUV→RGB is gamma-encoded; sRGB FB expects linear.
    outColor = vec4(srgb_to_linear(rgb), 1.0);
  } else if (pc.nv12 == 3) {
    // FSR / gamma RGBA video — same linearization as NV12 path.
    vec4 c = texture(videoTex, uv);
    if (c.a < 0.02) discard;
    outColor = vec4(srgb_to_linear(c.rgb), c.a);
  } else {
    // HUD RGBA (already authored for the sRGB swapchain).
    vec4 c = texture(videoTex, uv);
    if (c.a < 0.02) discard;
    outColor = c;
  }
}
