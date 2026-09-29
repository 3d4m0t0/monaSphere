#pragma once

#include <cstdint>

namespace vrp {

/** HUD quad vertex. Same layout as the scene mesh: position then UV. */
struct HudVertex {
  float x, y, z;
  float u, v;
};

}  // namespace vrp
