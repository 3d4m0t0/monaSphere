# vrp_starfield

All-sky star catalog + soft-disc mesh builder. **No Qt / Vulkan / OpenXR** — only the C++ standard library.

## Contents

| Piece | Path |
|-------|------|
| API | `starfield.hpp` / `starfield.cpp` |
| Catalog (Wikidata / CC0) | `data/stars_wikidata.bin` |
| Catalog notes | `data/README_stars.md` |
| Fetch / regenerate | `tools/fetch_wikidata_stars.py` |
| Example shaders | `shaders/star.vert`, `shaders/star.frag` |

## CMake

```cmake
add_subdirectory(path/to/monaSphere)  # or link the installed target
target_link_libraries(your_app PRIVATE vrp_starfield)
```

```cpp
#include "starfield/starfield.hpp"

auto stars = vrp::starfield::load_catalog("stars_wikidata.bin");
auto mesh = vrp::starfield::build_mesh(stars);  // 6 verts / star, triangle list
// Upload mesh to your GPU. Vertex layout: pos3, color3, uv2 (see Vertex).
// Draw with additive blending; fragment shader soft-circles via UV.
```

## License

- Library code: [MIT](../../LICENSE) (same as monaSphere)
- Star data: [CC0 1.0](../../data/LICENSE) (Wikidata); see [`data/README_stars.md`](../../data/README_stars.md)
- Combined package SPDX when shipping both: `MIT AND CC0-1.0`
