#!/usr/bin/env python3
"""Generate the cross-platform application icon pack for Pacific Squadron.

Source of truth: assets/app-icon-base.png (square, no alpha; its rounded-corner
mask is painted in black).

Outputs, all under assets/icons/:
  pacific-squadron-{16,24,32,48,64,128,256,512,1024}x{N}.png   Linux / generic
  pacific-squadron.ico                                          Windows
  pacific-squadron.icns                                         macOS

The .ico and .icns containers are written here directly, so the pack depends
only on Pillow and regenerates identically on Linux, macOS and Windows.

Usage:  python3 tools/make_icons.py [--source PATH] [--out DIR]
"""

from __future__ import annotations

import argparse
import io
import math
import struct
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # maintainer-only dependency; shipped icons are committed
    raise SystemExit(
        f"this script needs Pillow and numpy ({exc}).\n"
        "Install them with:  python3 -m pip install pillow numpy"
    )

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO / "assets" / "app-icon-base.png"
DEFAULT_OUT = REPO / "assets" / "icons"

# hicolor / Linux sizes, plus the extra sizes the containers need.
PNG_SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
# Apple ostype -> pixel size
ICNS_TYPES = (
    ("icp4", 16),    # 16x16
    ("icp5", 32),    # 32x32
    ("icp6", 64),    # 32x32 @2x
    ("ic07", 128),   # 128x128
    ("ic08", 256),   # 256x256
    ("ic09", 512),   # 512x512
    ("ic10", 1024),  # 512x512 @2x
    ("ic11", 32),    # 16x16 @2x
    ("ic12", 64),    # 32x32 @2x
    ("ic13", 256),   # 256x256 @2x
    ("ic14", 512),   # 512x512 @2x
)

# Corner radius of the master artwork as a fraction of the icon edge, fitted by
# least squares over the painted-black corner region of the 1254px master.
CORNER_RADIUS_FRAC = 0.1866
# Half-width of the alpha feather, in source pixels.
FEATHER = 0.9


# --------------------------------------------------------------------------- #
# Alpha extraction
# --------------------------------------------------------------------------- #
def rounded_box_alpha(size: int, radius_frac: float, feather: float) -> np.ndarray:
    """Analytic anti-aliased alpha for a centred rounded square (float32 0..1)."""
    half = size / 2.0
    r = radius_frac * size
    inner = half - r
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    qx = np.abs(x + 0.5 - half) - inner
    qy = np.abs(y + 0.5 - half) - inner
    outside = np.hypot(np.maximum(qx, 0.0), np.maximum(qy, 0.0))
    inside = np.minimum(np.maximum(np.minimum(qx, qy), 0.0), 0.0)
    dist = outside + inside - r
    # smoothstep across the feather band
    t = np.clip(0.5 - dist / (2.0 * feather), 0.0, 1.0)
    return (t * t * (3.0 - 2.0 * t)).astype(np.float32)


def decontaminate(rgb: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """Undo the master's baked-in blend against black.

    The master carries no alpha channel: its rounded corners are painted black,
    so the edge anti-aliasing is composited against black. Attaching real alpha
    while keeping those RGB values would leave a dark fringe on a light desktop;
    dividing by alpha recovers the un-premultiplied colour.
    """
    a = alpha[..., None]
    out = np.clip(rgb / np.maximum(a, 1.0 / 255.0), 0.0, 1.0)
    band = (alpha < 0.999)[..., None]
    return np.where(band, out, rgb).astype(np.float32)


def load_master(path: Path) -> np.ndarray:
    """Return the master as straight-alpha float32 RGBA in 0..1."""
    src = Image.open(path).convert("RGB")
    if src.width != src.height:
        raise SystemExit(f"master must be square, got {src.size}")
    rgb = np.asarray(src, dtype=np.float32) / 255.0
    alpha = rounded_box_alpha(src.width, CORNER_RADIUS_FRAC, FEATHER)
    return np.dstack([decontaminate(rgb, alpha), alpha])


# --------------------------------------------------------------------------- #
# Resampling
# --------------------------------------------------------------------------- #
def resize_premultiplied(rgba: np.ndarray, size: int) -> np.ndarray:
    """Lanczos resize in premultiplied space so edges never pull in black."""
    h, w = rgba.shape[:2]
    if (w, h) == (size, size):
        out = rgba.copy()
    else:
        a = rgba[..., 3:4]
        prem = np.dstack([rgba[..., :3] * a, a])
        img = Image.fromarray((prem * 255.0 + 0.5).astype(np.uint8), "RGBA")
        img = img.resize((size, size), Image.Resampling.LANCZOS)
        prem = np.asarray(img, dtype=np.float32) / 255.0
        out = np.dstack([prem[..., :3] / np.maximum(prem[..., 3:4], 1e-4), prem[..., 3:4]])
        out = np.clip(out, 0.0, 1.0)
    return (out * 255.0 + 0.5).astype(np.uint8)


def png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


# --------------------------------------------------------------------------- #
# Windows .ico
# --------------------------------------------------------------------------- #
def ico_entry_bmp(img: Image.Image) -> bytes:
    """32-bit BGRA DIB plus the 1-bit AND mask, bottom-up, as shells expect."""
    w, h = img.size
    px = np.asarray(img.convert("RGBA"), dtype=np.uint8)
    bgra = px[..., ::-1].copy()  # RGBA -> BGRA
    xor_data = b"".join(bgra[y].tobytes() for y in range(h - 1, -1, -1))

    # AND mask: opaque -> 0, transparent -> 1, rows padded to 32 bits.
    row_bytes = ((w + 31) // 32) * 4
    mask = np.zeros((h, row_bytes), dtype=np.uint8)
    transparent = px[..., 3] < 128
    for x in range(w):
        mask[transparent[:, x], x // 8] |= np.uint8(1 << (7 - (x % 8)))
    and_data = b"".join(mask[y].tobytes() for y in range(h - 1, -1, -1))

    header = struct.pack(
        "<IllHHIIllII",
        40,            # biSize
        w,             # biWidth
        h * 2,         # biHeight (XOR image + AND mask)
        1,             # biPlanes
        32,            # biBitCount
        0,             # biCompression = BI_RGB
        len(xor_data) + len(and_data),
        0, 0,          # resolution (unused)
        0, 0,          # colours used / important (unused)
    )
    return header + xor_data + and_data


def build_ico(images: dict[int, Image.Image], sizes: tuple[int, ...]) -> bytes:
    """ICONDIR + entries. PNG payload for 256px, BMP payload below that."""
    entries, payloads = [], []
    offset = 6 + 16 * len(sizes)
    for size in sizes:
        payload = png_bytes(images[size]) if size >= 256 else ico_entry_bmp(images[size])
        dim = 0 if size >= 256 else size  # a zero byte means 256
        entries.append(struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(payload), offset))
        payloads.append(payload)
        offset += len(payload)
    return struct.pack("<HHH", 0, 1, len(sizes)) + b"".join(entries) + b"".join(payloads)


# --------------------------------------------------------------------------- #
# macOS .icns
# --------------------------------------------------------------------------- #
def build_icns(images: dict[int, Image.Image]) -> bytes:
    """Apple Icon Container: 'icns' + big-endian length, then PNG entries."""
    body = bytearray()
    for ostype, size in ICNS_TYPES:
        payload = png_bytes(images[size])
        body += ostype.encode("ascii")
        body += struct.pack(">I", 8 + len(payload))
        body += payload
    return b"icns" + struct.pack(">I", 8 + len(body)) + bytes(body)


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the Pacific Squadron icon pack.")
    ap.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = ap.parse_args()

    if not args.source.exists():
        raise SystemExit(f"source not found: {args.source}")
    args.out.mkdir(parents=True, exist_ok=True)

    master = load_master(args.source)
    print(f"source : {args.source.name} {master.shape[1]}x{master.shape[0]} (alpha added)")

    needed = sorted(set(PNG_SIZES) | set(ICO_SIZES) | {s for _, s in ICNS_TYPES})
    images = {s: Image.fromarray(resize_premultiplied(master, s), "RGBA") for s in needed}

    for size in PNG_SIZES:
        name = f"pacific-squadron-{size}x{size}.png"
        images[size].save(args.out / name)
        print(f"  png   {name}")

    (args.out / "pacific-squadron.ico").write_bytes(build_ico(images, ICO_SIZES))
    print(f"  ico   pacific-squadron.ico  ({', '.join(map(str, ICO_SIZES))})")

    (args.out / "pacific-squadron.icns").write_bytes(build_icns(images))
    print(f"  icns  pacific-squadron.icns ({', '.join(t for t, _ in ICNS_TYPES)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
