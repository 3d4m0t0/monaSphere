# Star catalog (Wikidata / CC0)

Bright-star directions, visual magnitudes, and colors from Wikidata.
**License:** [CC0 1.0](LICENSE) ([deed](https://creativecommons.org/publicdomain/zero/1.0/)).

- Keep stars with apparent magnitude ≤ 5.0 (fainter stars omitted)
- Color from effective temperature (`P6879`) when present, else B−V color index (`P1458`) via Ballesteros (2012); else 6500 K
- File: little-endian magic `STR1`, `uint32` count, then `count` × `float32 x,y,z,mag,r,g,b` (unit direction: Y up, RA=0 toward −Z; rgb sRGB 0..1)
- Regenerate: `python3 tools/fetch_wikidata_stars.py`
