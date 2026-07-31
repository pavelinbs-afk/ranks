#!/usr/bin/env python3
"""Generate CS2-compatible skillgroup SVGs (Illustrator 32x13 subset).

CS2's VSVG compiler fails on Inkscape SVGs (relative paths, group transforms,
gradient defs). Gray outline-only previews = fills discarded.

These match vanilla skillgroup*.svg structure: Illustrator header, absolute
paths, solid fills, viewBox 0 0 32 13.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parent

# Rounded plate from Valve skillgroup1.svg
PLATE = (
    "M31.313,11.423c0,0.521-0.532,1.054-1.053,1.054H1.635"
    "c-0.52,0-0.943-0.424-0.943-0.945V1.521c0-0.521,0.424-0.945,0.943-0.945"
    "H30.26c0.521,0,1.053,0.315,1.053,0.836V11.423z"
)

# 7-segment digit geometry in local 0..1 box, drawn as absolute rects later
SEG = {
    "a": (0.15, 0.05, 0.70, 0.12),
    "b": (0.72, 0.15, 0.13, 0.32),
    "c": (0.72, 0.53, 0.13, 0.32),
    "d": (0.15, 0.83, 0.70, 0.12),
    "e": (0.15, 0.53, 0.13, 0.32),
    "f": (0.15, 0.15, 0.13, 0.32),
    "g": (0.15, 0.44, 0.70, 0.12),
}

DIGITS = {
    "0": "abcdef",
    "1": "bc",
    "2": "abged",
    "3": "abcdg",
    "4": "bcfg",
    "5": "afgcd",
    "6": "afgecd",
    "7": "abc",
    "8": "abcdefg",
    "9": "abcdfg",
}

# rank -> (bg, rim, digit) — vivid solids so Asset Browser shows color
PALETTE = [
    ("#1a3a6e", "#0d1f3d", "#7eb8ff"),  # 50
    ("#1e4478", "#102448", "#8ec0ff"),
    ("#225082", "#142a52", "#9ec8ff"),
    ("#0d5c4a", "#063528", "#5dffc8"),
    ("#0f6b56", "#073d32", "#6fffd0"),
    ("#127a62", "#08453c", "#81ffd8"),
    ("#6b4a12", "#3d2a08", "#ffd070"),
    ("#7a5615", "#46310a", "#ffd878"),
    ("#8a6218", "#4f380c", "#ffe080"),
    ("#6b1a1a", "#3d0e0e", "#ff9090"),  # 59
    ("#7a2020", "#461212", "#ffa0a0"),
    ("#8a2626", "#4f1515", "#ffb0b0"),
    ("#4a1a6b", "#2a0e3d", "#d090ff"),
    ("#562078", "#311246", "#d8a0ff"),
    ("#62268a", "#38154f", "#e0b0ff"),
    ("#1a4a6b", "#0e2a3d", "#90d0ff"),
    ("#205678", "#123146", "#a0d8ff"),
    ("#26628a", "#15384f", "#b0e0ff"),
    ("#6b5a1a", "#3d340e", "#ffe090"),
    ("#7a6820", "#463c12", "#ffe8a0"),
    ("#8a7626", "#4f4415", "#fff0b0"),  # 70
]


def rect_path(x: float, y: float, w: float, h: float) -> str:
    x2, y2 = x + w, y + h
    return f"M{x:.3f},{y:.3f}L{x2:.3f},{y:.3f}L{x2:.3f},{y2:.3f}L{x:.3f},{y2:.3f}Z"


def digit_paths(ch: str, ox: float, oy: float, dw: float, dh: float) -> list[str]:
    paths = []
    for seg in DIGITS[ch]:
        nx, ny, nw, nh = SEG[seg]
        paths.append(rect_path(ox + nx * dw, oy + ny * dh, nw * dw, nh * dh))
    return paths


def make_svg(rank: int, bg: str, digit: str) -> str:
    num = str(rank)
    # digit area inside plate
    dw, dh = 5.2, 8.5
    gap = 0.9
    total_w = len(num) * dw + (len(num) - 1) * gap
    start_x = (32.0 - total_w) / 2.0
    oy = (13.0 - dh) / 2.0

    parts = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generator: Adobe Illustrator 16.0.0, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->",
        '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">',
        '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px"',
        '\t width="32px" height="13px" viewBox="0 0 32 13" enable-background="new 0 0 32 13" xml:space="preserve">',
        f'\t<path fill="{bg}" d="{PLATE}"/>',
    ]

    for i, ch in enumerate(num):
        ox = start_x + i * (dw + gap)
        for d in digit_paths(ch, ox, oy, dw, dh):
            parts.append(f'\t<path fill="{digit}" d="{d}"/>')

    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def main() -> None:
    for i, colors in enumerate(PALETTE):
        rank = 50 + i
        bg, _rim, digit = colors
        text = make_svg(rank, bg, digit)
        path = OUT / f"skillgroup{rank}.svg"
        path.write_text(text, encoding="utf-8")
        print(f"wrote {path.name} ({len(text)} bytes)")


if __name__ == "__main__":
    main()
