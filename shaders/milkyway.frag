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

// Gaia EDR3 all-sky colour, galactic equirectangular (ℓ horizontal, centre ℓ≈0).
layout(set = 0, binding = 0) uniform sampler2D skyTex;

vec3 srgb_to_linear(vec3 c) {
  bvec3 cutoff = lessThanEqual(c, vec3(0.04045));
  vec3 low = c / 12.92;
  vec3 high = pow((c + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

vec3 galactic_centre_dir() {
  const float ra = radians(266.4051);
  const float dec = radians(-28.9362);
  float cl = cos(dec);
  return normalize(vec3(sin(ra) * cl, sin(dec), -cos(ra) * cl));
}

vec3 north_galactic_pole() {
  const float ra = radians(192.85948);
  const float dec = radians(27.12825);
  float cl = cos(dec);
  return normalize(vec3(sin(ra) * cl, sin(dec), -cos(ra) * cl));
}

void galactic_coords(vec3 d, out float b_deg, out float l_rad) {
  d = normalize(d);
  vec3 ngp = north_galactic_pole();
  vec3 gc = galactic_centre_dir();
  b_deg = degrees(asin(clamp(dot(d, ngp), -1.0, 1.0)));
  vec3 ge_x = normalize(gc - ngp * dot(gc, ngp));
  vec3 ge_y = cross(ngp, ge_x);
  l_rad = atan(dot(d, ge_y), dot(d, ge_x));
}

void main() {
  vec3 d = normalize(vDir);
  float b_deg;
  float l_rad;
  galactic_coords(d, b_deg, l_rad);
  float lon_deg = degrees(l_rad);

  // Equirect: u increases with decreasing ℓ (left = +ℓ), v=0 at +b.
  float u = fract(0.5 - lon_deg / 360.0);
  float v = clamp(0.5 - b_deg / 180.0, 0.0, 1.0);

  // atan/fract jump at ℓ=±180° blows implicit mip derivatives → bright/dark seam
  // line. Lod from direction footprint avoids that UV discontinuity.
  vec3 dd_x = dFdx(d);
  vec3 dd_y = dFdy(d);
  float ang = max(length(dd_x), length(dd_y));
  const float tex_w = 4000.0;
  float lod = max(0.0, log2(max(ang * (tex_w / 6.2831853), 1e-4)));
  vec3 c = textureLod(skyTex, vec2(u, v), lod).rgb;

  // Dust lanes are already dark in the map. Keep blacks low (no lift).
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float shade = pow(max(luma, 0.0), 1.28);
  vec3 tint = c * (shade / max(luma, 1e-4));

  float gain = max(pc.gain_mul, 0.05);
  // Soft overall — Gaia plate is bright; additive needs a low scale.
  float glow = 0.055 * shade * gain;
  glow = glow / (1.0 + 2.2 * glow);

  outColor = vec4(srgb_to_linear(tint) * glow, glow);
}
