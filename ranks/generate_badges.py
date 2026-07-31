#!/usr/bin/env python3
"""Generate CS2-compatible skillgroup SVGs matching vanilla rank format.

Vanilla MM ranks are ALWAYS:
  width="32px" height="13px" viewBox="0 0 32 13"

Custom 64x64 / square SVGs compile to black shield silhouettes in Asset Browser.

We reuse the vanilla plate outline (from skillgroup1) with solid fills + 7-seg digits.
"""

from __future__ import annotations

import pathlib

OUT = pathlib.Path(__file__).resolve().parent

# Exact outer plate from decompiled skillgroup1.svg (Silver I).
PLATE = (
    "M31.313,11.423c0,0.521-0.532,1.054-1.053,1.054H1.635"
    "c-0.52,0-0.943-0.424-0.943-0.945V1.521c0-0.521,0.424-0.945,"
    "0.943-0.945H30.26c0.521,0,1.053,0.315,1.053,0.836V11.423z"
)

COLORS = (
    ["#2563EB", "#3B82F6", "#60A5FA", "#38BDF8", "#0EA5E9"]
    + ["#059669", "#10B981", "#34D399", "#4ADE80", "#84CC16"]
    + ["#7C3AED", "#8B5CF6", "#A78BFA", "#C084FC", "#E879F9"]
    + ["#DC2626", "#EF4444", "#F97316", "#FB923C", "#FBBF24"]
    + ["#111827"]
)

SEGMENTS = {
    "0": "abcdef",
    "1": "bc",
    "2": "abged",
    "3": "abgcd",
    "4": "fgbc",
    "5": "afgcd",
    "6": "afgedc",
    "7": "abc",
    "8": "abcdefg",
    "9": "abfgcd",
    "P": "abefg",
}


def seg_paths(ox: float, oy: float, w: float, h: float, t: float, which: str) -> list[str]:
    mid = oy + h * 0.5
    segs = {
        "a": (ox + t, oy, w - 2 * t, t),
        "b": (ox + w - t, oy + t, t, h * 0.5 - 1.5 * t),
        "c": (ox + w - t, mid + 0.5 * t, t, h * 0.5 - 1.5 * t),
        "d": (ox + t, oy + h - t, w - 2 * t, t),
        "e": (ox, mid + 0.5 * t, t, h * 0.5 - 1.5 * t),
        "f": (ox, oy + t, t, h * 0.5 - 1.5 * t),
        "g": (ox + t, mid - 0.5 * t, w - 2 * t, t),
    }
    out = []
    for name in which:
        x, y, rw, rh = segs[name]
        out.append(
            f"M{x:.3f},{y:.3f}L{x + rw:.3f},{y:.3f}L{x + rw:.3f},{y + rh:.3f}L{x:.3f},{y + rh:.3f}z"
        )
    return out


def make_svg(label: str, color: str) -> str:
    chars = list(label)
    n = len(chars)
    # Digits must fit inside the 32x13 plate with padding.
    cell_w = 4.2 if n >= 2 else 5.5
    cell_h = 9.0
    thick = 1.15
    gap = 0.9
    total = n * cell_w + (n - 1) * gap
    x0 = (32.0 - total) * 0.5
    y0 = (13.0 - cell_h) * 0.5

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generator: lr_core ranks/generate_badges.py — CS2 32x13 skillgroup -->",
        '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" '
        '"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">',
        '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px" '
        'width="32px" height="13px" viewBox="0 0 32 13" '
        'enable-background="new 0 0 32 13" xml:space="preserve">',
        f'\t<path fill="{color}" d="{PLATE}"/>',
    ]

    for i, ch in enumerate(chars):
        which = SEGMENTS.get(ch, SEGMENTS["0"])
        ox = x0 + i * (cell_w + gap)
        for d in seg_paths(ox, y0, cell_w, cell_h, thick, which):
            lines.append(f'\t<path fill="#FFFFFF" d="{d}"/>')

    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for i, color in enumerate(COLORS):
        skill_id = 50 + i
        label = "P" if skill_id == 70 else str(i + 1)
        path = OUT / f"skillgroup{skill_id}.svg"
        path.write_text(make_svg(label, color), encoding="utf-8", newline="\n")
        print("wrote", path.name, f"({label}, {color})")

    # Control sample: recolored vanilla plate only — must NOT be a black shield.
    probe = OUT / "skillgroup999999.svg"
    probe.write_text(make_svg("8", "#FF00AA"), encoding="utf-8", newline="\n")
    print("wrote", probe.name, "(probe — delete before publish if unused)")
    print()
    print("CRITICAL: size must stay 32x13 like vanilla. Square SVGs = black shields.")
    print("Copy ONLY into content/csgo_addons/<addon>/panorama/images/icons/skillgroups/")
    print("Delete matching files under game/csgo_addons/... so Tools recompile.")


if __name__ == "__main__":
    main()
