# Milky Way backdrop (Gaia EDR3 colour equirectangular)

monaSphere draws the galactic sky by sampling `gaia_sky_edr3_equirect.jpg` on an
inward sphere (`shaders/milkyway.vert` / `milkyway.frag`), additive-blended under
the star / nebula layers.

## Spec (locked)

| Item | Value |
|------|--------|
| Projection | **Cartesian / equirectangular** (filled 2:1 rectangle) |
| Coordinates | Galactic: plane horizontal, ℓ≈0 at image centre, +ℓ toward the left |
| Runtime file | `data/gaia_sky_edr3_equirect.jpg` (copied next to `shaders/` at build) |
| Source asset | ESA `Gaia_EDR3_flux_cartesian_{2k,4k,16k}.png` |
| Refresh | `python3 tools/fetch_gaia_sky.py` (default `--size 4k`) |
| Licence | [CC BY-SA 3.0 IGO](https://creativecommons.org/licenses/by-sa/3.0/igo/) — ESA/Gaia/DPAC; acknowledgement A. Moitinho |

**Do not** use Hammer / oval plates (`Gaia_EDR3_flux_hammer_*`, Wikimedia
`ESA22358060`, or `*black_bg*` all-sky art). Those leave empty corners; mapping
them as lon/lat puts the left/right tips at the poles and opens black gaps.

## Runtime processing

1. Load JPEG → RGBA (FFmpeg). No CPU denoise (plate is pre-cleaned).
2. Seal ℓ=±180° (cosine band + identical wrap columns) for `REPEAT` sampling.
3. Upload with a full mip chain; fragment shader uses direction-based `textureLod`
   so the longitude cut does not spike automatic mip derivatives, then applies
   shade / soft additive glow scaled by `milkyway_gain`.
4. Gain defaults to `1.0` via `monasphere.conf` `milkyway_gain` (no in-app control).
5. Earth-horizon orientation from the **host timezone** (`sky_from_timezone=1`, default):
   lat/lon from `/usr/share/zoneinfo/zone*.tab` for the system IANA zone.
   Set `sky_from_timezone=0` to keep manual `sky_lat` / `sky_lon`.
   Recenter facing **true north**: local up = +Y, north = −Z (flat screen on −Z).
   Sky directions ignore Earth radius; only the horizon tip uses latitude (sphere).
   LST from lon + UTC rotates the sky.

Point stars remain `stars_wikidata.bin`; this texture is the soft density backdrop only.
The Sun and Moon are drawn from a low-precision ephemeris on the host clock (LST +
body motion).

## Attribution

When redistributing binaries that include `gaia_sky_edr3_equirect.jpg`, retain
ESA/Gaia credit and the CC BY-SA 3.0 IGO notice (see [`THIRD_PARTY.md`](../THIRD_PARTY.md)).
