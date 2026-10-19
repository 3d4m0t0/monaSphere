#!/usr/bin/env python3
"""Download Gaia EDR3 all-sky colour as true equirectangular JPEG for monaSphere.

ESA publishes two projections — only **cartesian** is valid for the sky sphere:

  - Cartesian / equirect  — Gaia_EDR3_flux_cartesian_*.png  ← this tool
  - Hammer (oval on black) — Gaia_EDR3_flux_hammer_*.png    ← do not use

Licence: CC BY-SA 3.0 IGO — ESA/Gaia/DPAC; acknowledgement A. Moitinho.

Writes: data/gaia_sky_edr3_equirect.jpg

See data/README_milkyway.md.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JPG_OUT = ROOT / "data" / "gaia_sky_edr3_equirect.jpg"
UA = "monaSphere-gaia-fetch/1.0 (https://github.com/3d4m0t0/monaSphere)"

CARTESIAN = {
    "2k": "https://sci.esa.int/documents/33580/35361/Gaia_EDR3_flux_cartesian_2k.png",
    "4k": "https://sci.esa.int/documents/33580/35361/Gaia_EDR3_flux_cartesian_4k.png",
    "16k": "https://sci.esa.int/documents/33580/35361/Gaia_EDR3_flux_cartesian_16k.png",
}


def download(url: str, dest: Path) -> None:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    print(f"Downloading {url}")
    with urllib.request.urlopen(req, timeout=600) as resp:
        data = resp.read()
    if len(data) < 10_000:
        raise RuntimeError(f"Download too small ({len(data)} bytes)")
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    print(f"Wrote {dest} ({len(data)} bytes)")


def to_jpeg(src: Path, jpg: Path) -> None:
    subprocess.run(
        ["ffmpeg", "-y", "-i", str(src), "-frames:v", "1", "-update", "1", "-q:v", "2", str(jpg)],
        check=True,
        capture_output=True,
    )
    print(f"Wrote {jpg}")


def probe_rgba(jpg: Path) -> tuple[int, int, bytes]:
    probe = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=width,height",
            "-of",
            "csv=p=0:s=x",
            str(jpg),
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    w_s, h_s = probe.stdout.strip().split("x")
    w, h = int(w_s), int(h_s)
    with tempfile.NamedTemporaryFile(suffix=".rgba", delete=False) as tmp:
        raw_path = Path(tmp.name)
    try:
        subprocess.run(
            ["ffmpeg", "-y", "-i", str(jpg), "-f", "rawvideo", "-pix_fmt", "rgba", str(raw_path)],
            check=True,
            capture_output=True,
        )
        rgba = raw_path.read_bytes()
    finally:
        raw_path.unlink(missing_ok=True)
    expect = w * h * 4
    if len(rgba) != expect:
        raise RuntimeError(f"RGBA size {len(rgba)} != {expect}")
    return w, h, rgba


def assert_equirect_filled(jpg: Path) -> None:
    """Reject Hammer/oval plates that leave empty corners."""
    w, h, rgba = probe_rgba(jpg)

    def lum(x: int, y: int) -> float:
        i = (y * w + x) * 4
        return (rgba[i] + rgba[i + 1] + rgba[i + 2]) / 3.0

    y = max(1, h // 20)
    left = lum(w // 8, y)
    right = lum(w - 1 - w // 8, y)
    mid = lum(w // 2, h // 2)
    if mid < 40:
        raise RuntimeError(f"Centre too dark ({mid:.1f}); not a Gaia flux plate?")
    if left < 2.0 and right < 2.0:
        raise RuntimeError(
            "Image looks like Hammer/oval (empty upper corners). "
            "Use Gaia_EDR3_flux_cartesian_*.png only."
        )
    if abs(w / h - 2.0) > 0.05:
        raise RuntimeError(f"Expected ~2:1 equirect, got {w}x{h}")
    print(f"Equirect check OK ({w}x{h}, top-side luma L={left:.1f} R={right:.1f})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--jpg", type=Path, default=JPG_OUT)
    ap.add_argument("--size", choices=sorted(CARTESIAN.keys()), default="4k")
    ap.add_argument("--full", action="store_true", help="Alias for --size 16k")
    ap.add_argument("--from-image", type=Path, help="Skip download; convert existing PNG/JPEG")
    args = ap.parse_args()
    if args.full:
        args.size = "16k"

    if args.from_image is None:
        with tempfile.TemporaryDirectory() as td:
            png = Path(td) / "cartesian.png"
            download(CARTESIAN[args.size], png)
            to_jpeg(png, args.jpg)
    else:
        src = args.from_image
        if src.suffix.lower() == ".png":
            to_jpeg(src, args.jpg)
        elif src.suffix.lower() in {".jpg", ".jpeg"}:
            args.jpg.parent.mkdir(parents=True, exist_ok=True)
            args.jpg.write_bytes(src.read_bytes())
        else:
            print("Unsupported --from-image type", file=sys.stderr)
            return 1

    try:
        assert_equirect_filled(args.jpg)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
