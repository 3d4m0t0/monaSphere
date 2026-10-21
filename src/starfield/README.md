# vrp_starfield

All-sky star catalog + soft-disc mesh builder. **No Qt / Vulkan / OpenXR** — only the C++ standard library.

## Contents

| Piece | Path |
|-------|------|
| API | `starfield.hpp` / `starfield.cpp` |
| Star catalog (Wikidata / CC0) | `data/stars_wikidata.bin` |
| Nebula catalog (Wikidata / CC0) | `data/nebulae_wikidata.bin` |
| Catalog notes | `data/README_stars.md`, `data/README_nebulae.md` |
| Fetch / regenerate | `tools/fetch_wikidata_stars.py`, `tools/fetch_wikidata_nebulae.py` |
| Example shaders | `shaders/star.*`, `shaders/nebula.*`, `shaders/moon.*` |
| Sky sphere helper | `build_sky_sphere()` — used by the app with Gaia milkyway shaders |

The Milky Way **texture** (Gaia EDR3 equirect) lives in the player, not in this
library. See [`data/README_milkyway.md`](../../data/README_milkyway.md).

## CMake

```cmake
add_subdirectory(path/to/monaSphere)  # or link the installed target
target_link_libraries(your_app PRIVATE vrp_starfield)
```

```cpp
#include "starfield/starfield.hpp"

auto stars = vrp::starfield::load_catalog("stars_wikidata.bin");
auto mesh = vrp::starfield::build_mesh(stars);
auto [sky_v, sky_i] = vrp::starfield::build_sky_sphere(98.f);
float R[16];
earth_sky_matrix(unix_utc, /*lat*/35.68f, /*lon*/139.65f, R);
// App: multiply sky model by R (−Z = north, NCP altitude = lat, LST rotates sky).
```

## License

- Library code: [MIT](../../LICENSE) (same as monaSphere)
- Star / nebula data: [CC0 1.0](../../data/LICENSE) (Wikidata)
- Combined package SPDX when shipping catalogs: `MIT AND CC0-1.0`
  (add `AND CC-BY-SA-3.0-IGO` if shipping the Gaia sky JPEG with the app)
