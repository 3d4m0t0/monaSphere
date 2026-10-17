#!/usr/bin/env python3
"""Fetch bright-star RA/Dec/mag/color from Wikidata (CC0) → data/stars_wikidata.bin."""
from __future__ import annotations

import csv
import io
import math
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "stars_wikidata.bin"
README = ROOT / "data" / "README_stars.md"

# Keep stars of visual magnitude MAG_MAX or brighter (lower number = brighter).
MAG_MAX = 5.0

ENDPOINT = "https://query.wikidata.org/sparql"
MAGIC = 0x31525453  # 'STR1' LE
UA = "monaSphere-star-fetch/1.0 (https://github.com/3d4m0t0/monaSphere)"


def sparql_rows(query: str) -> list[dict[str, str]]:
    """Run a SPARQL SELECT via curl (WDQS returns CSV reliably with Accept)."""
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
    if not text or "<sparql" in text[:200].lower() or "ra" not in text.splitlines()[0].lower():
        raise RuntimeError(f"SPARQL did not return CSV (head={text[:80]!r})")
    return list(csv.DictReader(io.StringIO(text)))


def bv_to_teff(bv: float) -> float:
    """Ballesteros (2012) B−V → T_eff (K)."""
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def teff_to_rgb(kelvin: float) -> tuple[float, float, float]:
    """Approximate blackbody chromaticity (Tanner Helland style), 0..1 sRGB."""
    t = max(1000.0, min(40000.0, kelvin)) / 100.0
    if t <= 66.0:
        r = 1.0
        g = max(0.0, min(1.0, (99.4708025861 * math.log(t) - 161.1195681661) / 255.0))
    else:
        r = max(0.0, min(1.0, (329.698727446 * (t - 60.0) ** -0.1332047592) / 255.0))
        g = max(0.0, min(1.0, (288.1221695283 * (t - 60.0) ** -0.0755148492) / 255.0))
    if t >= 66.0:
        b = 1.0
    elif t <= 19.0:
        b = 0.0
    else:
        b = max(0.0, min(1.0, (138.5177312231 * math.log(t - 10.0) - 305.0447927307) / 255.0))
    m = max(r, g, b, 1e-5)
    return (r / m, g / m, b / m)


def star_rgb(bv: float | None, teff: float | None) -> tuple[float, float, float]:
    if teff is not None and teff > 500.0:
        return teff_to_rgb(teff)
    if bv is not None and -0.5 <= bv <= 2.5:
        return teff_to_rgb(bv_to_teff(bv))
    return teff_to_rgb(6500.0)


def main() -> None:
    # Split queries: base positions are large; color is optional enrichment.
    base_q = f"""
SELECT ?ra ?dec ?mag WHERE {{
  ?s wdt:P31/wdt:P279* wd:Q523;
     wdt:P6257 ?ra;
     wdt:P6258 ?dec;
     wdt:P1215 ?mag.
  FILTER(?mag <= {MAG_MAX})
}}
"""
    # Color lookups avoid the heavy star-class path (P31/P279*).
    teff_q = f"""
SELECT ?ra ?dec ?teff WHERE {{
  ?s wdt:P6879 ?teff;
     wdt:P6257 ?ra;
     wdt:P6258 ?dec;
     wdt:P1215 ?mag.
  FILTER(?mag <= {MAG_MAX})
}}
"""
    bv_q = f"""
SELECT ?ra ?dec ?bv WHERE {{
  ?s wdt:P1458 ?bv;
     wdt:P6257 ?ra;
     wdt:P6258 ?dec;
     wdt:P1215 ?mag.
  FILTER(?mag <= {MAG_MAX})
}}
"""

    print("Fetching positions…")
    base_rows = sparql_rows(base_q)
    print(f"  {len(base_rows)} rows")

    colors: dict[tuple[float, float], tuple[float | None, float | None]] = {}
    print("Fetching Teff…")
    try:
        teff_rows = sparql_rows(teff_q)
        print(f"  {len(teff_rows)} Teff rows")
        for row in teff_rows:
            try:
                ra = float(row["ra"])
                dec = float(row["dec"])
                teff = float(row["teff"])
            except (KeyError, ValueError):
                continue
            if teff < 2000.0 or teff > 50000.0:
                continue
            key = (round(ra, 4), round(dec, 4))
            bv_prev, teff_prev = colors.get(key, (None, None))
            if teff_prev is None:
                colors[key] = (bv_prev, teff)
    except (subprocess.CalledProcessError, RuntimeError, OSError) as e:
        print(f"Teff query failed ({e})")

    print("Fetching B−V…")
    try:
        bv_rows = sparql_rows(bv_q)
        print(f"  {len(bv_rows)} B−V rows")
        for row in bv_rows:
            try:
                ra = float(row["ra"])
                dec = float(row["dec"])
                bv = float(row["bv"])
            except (KeyError, ValueError):
                continue
            if not (-0.5 <= bv <= 2.5):
                continue
            key = (round(ra, 4), round(dec, 4))
            bv_prev, teff_prev = colors.get(key, (None, None))
            if bv_prev is None:
                colors[key] = (bv, teff_prev)
    except (subprocess.CalledProcessError, RuntimeError, OSError) as e:
        print(f"B−V query failed ({e})")

    best: dict[tuple[float, float], tuple[float, float, float]] = {}
    for row in base_rows:
        try:
            ra = float(row["ra"])
            dec = float(row["dec"])
            mag = float(row["mag"])
        except (KeyError, ValueError):
            continue
        if mag < -1.6 or mag > MAG_MAX:
            continue
        if not (-90.0 <= dec <= 90.0) or not (0.0 <= ra < 360.0):
            continue
        key = (round(ra, 4), round(dec, 4))
        if key not in best or mag < best[key][2]:
            best[key] = (ra, dec, mag)

    stars = sorted(best.values(), key=lambda t: t[2])
    colored = 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as f:
        f.write(struct.pack("<II", MAGIC, len(stars)))
        for ra, dec, mag in stars:
            lat = math.radians(dec)
            lon = math.radians(ra)
            cl = math.cos(lat)
            x = math.sin(lon) * cl
            y = math.sin(lat)
            z = -math.cos(lon) * cl
            bv, teff = colors.get((round(ra, 4), round(dec, 4)), (None, None))
            if bv is not None or teff is not None:
                colored += 1
            r, g, b = star_rgb(bv, teff)
            f.write(struct.pack("<fffffff", x, y, z, mag, r, g, b))

    if len(stars) == 0:
        raise SystemExit("No stars fetched; refusing to overwrite with empty catalog")

    README.write_text(
        "# Star catalog (Wikidata / CC0)\n\n"
        "Bright-star directions, visual magnitudes, and colors from Wikidata "
        "([CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)).\n\n"
        f"- Keep stars with apparent magnitude ≤ {MAG_MAX} (fainter stars omitted)\n"
        "- Color from effective temperature (`P6879`) when present, else B−V color index "
        "(`P1458`) via Ballesteros (2012); else 6500 K\n"
        "- File: little-endian magic `STR1`, `uint32` count, then `count` × "
        "`float32 x,y,z,mag,r,g,b` (unit direction: Y up, RA=0 toward −Z; rgb sRGB 0..1)\n"
        "- Regenerate: `python3 tools/fetch_wikidata_stars.py`\n"
    )
    print(f"Wrote {len(stars)} stars ({colored} with Teff/B−V) to {OUT}")


if __name__ == "__main__":
    main()
