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

1. Load JPEG → RGBA (FFmpeg).
2. Edge-preserving denoise (asymmetric residual shrink + wide low-pass fill) so
   star-density grain collapses into a continuous glow while dark dust lanes stay.
3. Seal ℓ=±180° (cosine band + identical wrap columns) for `REPEAT` sampling.
4. Upload with a full mip chain; fragment shader uses direction-based `textureLod`
   so the longitude cut does not spike automatic mip derivatives.
5. Gain defaults to `1.0` via `monasphere.conf` `milkyway_gain` (no in-app control).

Point stars remain `stars_wikidata.bin`; this texture is the soft density backdrop only.

## Attribution

When redistributing binaries that include `gaia_sky_edr3_equirect.jpg`, retain
ESA/Gaia credit and the CC BY-SA 3.0 IGO notice (see [`THIRD_PARTY.md`](../THIRD_PARTY.md)).
