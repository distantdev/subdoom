"""Render Subdoom app / plugin icon PNGs.

Same white tile and three-bar language as Doomgazer, with the mass inverted:
the outer bars stand tall and the center sits low, so the silhouette dips.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

BLACK = (0x00, 0x00, 0x00, 255)
WHITE = (0xFA, 0xF8, 0xF5, 255)
EDGE = (0xD8, 0xD4, 0xCC, 255)

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "plugin" / "assets"


def _snap(value: float, grid: float = 1.0) -> float:
    return round(value / grid) * grid


def _bar(draw: ImageDraw.ImageDraw, x0: float, x1: float, y_base: float, y_left: float, y_right: float) -> None:
    draw.polygon(
        [
            (int(round(x0)), int(round(y_base))),
            (int(round(x1)), int(round(y_base))),
            (int(round(x1)), int(round(y_right))),
            (int(round(x0)), int(round(y_left))),
        ],
        fill=BLACK,
    )


def _mirror(img: Image.Image) -> Image.Image:
    width, height = img.size
    pixels = img.load()
    for y in range(height):
        for x in range(width // 2):
            pixels[width - 1 - x, y] = pixels[x, y]
    return img


def draw_icon(size: int) -> Image.Image:
    if size > 48:
        ref = size if size >= 1024 else size * 2
        img = _draw(ref)
        if ref != size:
            img = img.resize((size, size), Image.Resampling.LANCZOS)
        return _mirror(img)
    return _mirror(_draw(size))


def _draw(size: int) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    margin = max(1, size // 16)
    radius = max(2, size // 5)
    draw.rounded_rectangle(
        (margin, margin, size - margin - 1, size - margin - 1),
        radius=radius,
        fill=WHITE,
        outline=EDGE,
        width=max(1, size // 64),
    )

    tile_cy = (margin + size - margin) * 0.5
    cx = size // 2
    top_f = 0.36
    dip_f = 0.62
    base_f = 0.935
    hem_f = 0.034
    shape_cy = size * (top_f + base_f) * 0.5
    shift = tile_cy - shape_cy

    y_base = _snap(size * (base_f + hem_f) + shift)
    y_top = _snap(size * top_f + shift)
    y_dip = _snap(size * dip_f + shift)
    if size <= 48:
        nudge = int(round(((size - 1) * 0.5) - ((y_top + y_base) * 0.5)))
        y_base += nudge
        y_top += nudge
        y_dip += nudge

    span = int(round(size * 0.64))
    if span % 2:
        span -= 1
    gap = max(2, int(round(span * 0.14)))
    pitch = max(2, (span - 2 * gap) // 3)
    leftover = span - (3 * pitch + 2 * gap)
    gap += leftover // 2
    x0 = cx - (3 * pitch + 2 * gap) // 2

    for index in range(3):
        left = x0 + index * (pitch + gap)
        right = left + pitch
        if index == 0:
            _bar(draw, float(left), float(right), y_base, y_top, y_dip)
        elif index == 1:
            _bar(draw, float(left), float(right), y_base, y_dip, y_dip)
        else:
            _bar(draw, float(left), float(right), y_base, y_dip, y_top)
    return img


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for name, px in (("subdoom_icon_512.png", 512), ("subdoom_icon_32.png", 32)):
        path = OUT / name
        draw_icon(px).save(path, optimize=True)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
