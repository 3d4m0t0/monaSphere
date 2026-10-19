#!/usr/bin/env python3
"""Fetch nebulae RA/Dec from Wikidata (CC0) → data/nebulae_wikidata.bin.

Colors are assigned by nebula class (emission / reflection / planetary / …)
so chromatic identity stays strong in the soft-disc renderer.
"""
from __future__ import annotations

import csv
import io
import math
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "nebulae_wikidata.bin"
README = ROOT / "data" / "README_nebulae.md"

ENDPOINT = "https://query.wikidata.org/sparql"
MAGIC = 0x314B5344  # 'DSK1' LE
UA = "monaSphere-nebula-fetch/1.0 (https://github.com/3d4m0t0/monaSphere)"

# (qid, kind key, sRGB chromaticity, default mag, size bias)
KINDS = [
    ("Q202265", "emission", (1.00, 0.28, 0.42), 6.5, 1.25),  # Hα pink-red
    ("Q11282", "hii", (1.00, 0.32, 0.48), 6.8, 1.35),  # H II
    ("Q13632", "planetary", (0.35, 1.00, 0.72), 8.0, 0.75),  # OIII teal
    ("Q203958", "reflection", (0.40, 0.58, 1.00), 7.5, 1.0),  # blue dust
    ("Q207436", "snr", (1.00, 0.55, 0.28), 7.0, 1.15),  # orange
    ("Q42372", "nebula", (0.85, 0.45, 1.00), 7.2, 1.0),  # magenta
]


def sparql_rows(query: str) -> list[dict[str, str]]:
    proc = subprocess.run(
        [
            "curl",
            "-sS",
            "-G",
            ENDPOINT,
            "-H",
            f"User-Agent: {UA}",
            "-H",
            "Accept: text/csv",
            "--data-urlencode",
            f"query={query}",
            "--max-time",
            "300",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    text = proc.stdout
    if not text or "ra" not in text.splitlines()[0].lower():
        raise RuntimeError(f"SPARQL did not return CSV (head={text[:100]!r})")
    return list(csv.DictReader(io.StringIO(text)))


def ra_dec_to_dir(ra_deg: float, dec_deg: float) -> tuple[float, float, float]:
    ra = math.radians(ra_deg)
    dec = math.radians(dec_deg)
    cl = math.cos(dec)
    # Y up, RA=0 toward −Z (monaSphere / star catalog).
    return (math.sin(ra) * cl, math.sin(dec), -math.cos(ra) * cl)


def saturate(rgb: tuple[float, float, float], amount: float = 1.35) -> tuple[float, float, float]:
    r, g, b = rgb
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    r = max(0.0, min(1.0, luma + (r - luma) * amount))
    g = max(0.0, min(1.0, luma + (g - luma) * amount))
    b = max(0.0, min(1.0, luma + (b - luma) * amount))
    m = max(r, g, b, 1e-5)
    return (r / m, g / m, b / m)


def main() -> None:
    # kind priority: earlier KINDS win on duplicates.
    priority = {k: i for i, (k, *_rest) in enumerate(KINDS)}
    kind_meta = {k: (name, rgb, mag, size) for k, name, rgb, mag, size in KINDS}

    best: dict[tuple[float, float], tuple[int, float, float, float, float, float, float, float]] = {}
    # value: (prio, x,y,z, mag, r,g,b) — mag encodes brightness; size bias folded into mag.

    for qid, name, rgb, def_mag, size_bias in KINDS:
        q = f"""
SELECT ?ra ?dec ?mag WHERE {{
  ?o wdt:P31 wd:{qid};
     wdt:P6257 ?ra;
     wdt:P6258 ?dec.
  ?article schema:about ?o; schema:isPartOf <https://en.wikipedia.org/>.
  OPTIONAL {{ ?o wdt:P1215 ?mag. }}
}}
"""
        print(f"Fetching {name} ({qid})…")
        rows = sparql_rows(q)
        print(f"  {len(rows)} rows")
        rgb_s = saturate(rgb, 1.45)
        prio = priority[qid]
        for row in rows:
            try:
                ra = float(row["ra"])
                dec = float(row["dec"])
            except (KeyError, ValueError):
                continue
            mag = def_mag
            if row.get("mag"):
                try:
                    m = float(row["mag"])
                    # Apparent magnitudes only; reject abs-mag / garbage.
                    if 0.0 <= m <= 14.0:
                        mag = m
                except ValueError:
                    pass
            # Brighter proxy for larger classes (lower mag number).
            mag = mag - 0.35 * (size_bias - 1.0)
            mag = max(2.5, min(11.5, mag))
            x, y, z = ra_dec_to_dir(ra, dec)
            key = (round(ra, 4), round(dec, 4))
            cur = best.get(key)
            if cur is None or prio < cur[0] or (prio == cur[0] and mag < cur[4]):
                best[key] = (prio, x, y, z, mag, rgb_s[0], rgb_s[1], rgb_s[2])

    items = sorted(best.values(), key=lambda t: t[4])
    print(f"Unique nebulae: {len(items)}")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as f:
        f.write(struct.pack("<II", MAGIC, len(items)))
        for _p, x, y, z, mag, r, g, b in items:
            f.write(struct.pack("<fffffff", x, y, z, mag, r, g, b))
    print(f"Wrote {OUT} ({OUT.stat().st_size} bytes)")

    README.write_text(
        """# Nebula catalog (Wikidata / CC0)

Deep-sky nebulae with equatorial coordinates from Wikidata.
**License:** [CC0 1.0](LICENSE) ([deed](https://creativecommons.org/publicdomain/zero/1.0/)).

- English Wikipedia sitelink required (keeps the set to well-known objects)
- Classes: emission, H II, planetary, reflection, supernova remnant, generic nebula
- Colors are **class chromaticities** (saturated), not measured photometry — chosen so
  emission reads pink/red, planetary teal, reflection blue, SNR orange
- File: little-endian magic `DSK1`, `uint32` count, then `count` ×
  `float32 x,y,z,mag,r,g,b` (same layout as the star catalog)
- Regenerate: `python3 tools/fetch_wikidata_nebulae.py`
""",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
