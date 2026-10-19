# Nebula catalog (Wikidata / CC0)

Deep-sky nebulae with equatorial coordinates from Wikidata.
**License:** [CC0 1.0](LICENSE) ([deed](https://creativecommons.org/publicdomain/zero/1.0/)).

- English Wikipedia sitelink required (keeps the set to well-known objects)
- Classes: emission, H II, planetary, reflection, supernova remnant, generic nebula
- Colors are **class chromaticities** (saturated), not measured photometry — chosen so
  emission reads pink/red, planetary teal, reflection blue, SNR orange
- File: little-endian magic `DSK1`, `uint32` count, then `count` ×
  `float32 x,y,z,mag,r,g,b` (same layout as the star catalog)
- Regenerate: `python3 tools/fetch_wikidata_nebulae.py`
