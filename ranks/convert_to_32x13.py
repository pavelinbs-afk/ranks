#!/usr/bin/env python3
"""Flatten Inkscape skillgroup SVGs for CS2 Workshop Tools.

CS2's SVG→VSVG compiler does NOT apply fill=\"url(#gradient)\". Result: only
thin gray strokes remain in Asset Browser. This script:

  1. resolves linearGradient chains to a solid hex color
  2. replaces fill=\"url(#...)\" with that solid color
  3. drops <defs> (gradients unused after flatten)
  4. sets display size to vanilla 32x13 (keeps original viewBox)
  5. keeps fill=\"none\" on true outline strokes
"""

from __future__ import annotations

import re
from pathlib import Path

OUT = Path(__file__).resolve().parent
BACKUP = OUT / "_backup_before_32x13"

SVG_OPEN_RE = re.compile(r"<svg\b[^>]*>", re.IGNORECASE | re.DOTALL)
VIEWBOX_RE = re.compile(r'viewBox\s*=\s*"([^"]+)"', re.IGNORECASE)
DEFS_RE = re.compile(r"<defs\b[^>]*>.*?</defs>", re.IGNORECASE | re.DOTALL)
GRAD_RE = re.compile(
    r'<linearGradient\b([^>]*)>(.*?)</linearGradient>',
    re.IGNORECASE | re.DOTALL,
)
STOP_RE = re.compile(r"<stop\b([^>]*)/?>", re.IGNORECASE)
ATTR_RE = re.compile(r'([\w:.-]+)\s*=\s*"([^"]*)"')
PATH_RE = re.compile(r"<path\b[^>]*/?>", re.IGNORECASE)
URL_FILL_RE = re.compile(r'fill\s*=\s*"url\(#([^)]+)\)"', re.IGNORECASE)


def parse_attrs(s: str) -> dict[str, str]:
    return {m.group(1): m.group(2) for m in ATTR_RE.finditer(s)}


def parse_hex(color: str | None) -> tuple[int, int, int] | None:
    if not color:
        return None
    color = color.strip()
    if color.startswith("#") and len(color) == 4:
        color = "#" + "".join(c * 2 for c in color[1:])
    if color.startswith("#") and len(color) == 7:
        return int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16)
    if color.lower() in ("white", "#fff", "#ffffff"):
        return 255, 255, 255
    if color.lower() in ("black", "#000", "#000000"):
        return 0, 0, 0
    return None


def to_hex(rgb: tuple[int, int, int]) -> str:
    return f"#{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X}"


def avg_colors(colors: list[tuple[int, int, int]]) -> tuple[int, int, int]:
    if not colors:
        return (128, 128, 128)
    n = len(colors)
    return (
        sum(c[0] for c in colors) // n,
        sum(c[1] for c in colors) // n,
        sum(c[2] for c in colors) // n,
    )


def build_gradient_map(svg: str) -> dict[str, str]:
    """id -> solid #RRGGBB, resolving xlink:href chains."""
    raw: dict[str, dict] = {}
    for m in GRAD_RE.finditer(svg):
        attrs = parse_attrs(m.group(1))
        gid = attrs.get("id")
        if not gid:
            continue
        href = attrs.get("xlink:href") or attrs.get("href")
        stops: list[tuple[int, int, int]] = []
        for sm in STOP_RE.finditer(m.group(2)):
            sa = parse_attrs(sm.group(1))
            rgb = parse_hex(sa.get("stop-color"))
            if rgb is None and "stop-color" not in sa:
                rgb = (0, 0, 0)  # SVG default for bare <stop>
            if rgb is not None:
                stops.append(rgb)
        raw[gid] = {"href": href.lstrip("#") if href else None, "stops": stops}

    memo: dict[str, tuple[int, int, int]] = {}

    def resolve(gid: str, stack: set[str] | None = None) -> tuple[int, int, int]:
        if gid in memo:
            return memo[gid]
        stack = stack or set()
        if gid in stack or gid not in raw:
            return (128, 128, 128)
        stack.add(gid)
        info = raw[gid]
        if info["stops"]:
            # Prefer the brighter stop so badges stay readable at 32x13.
            color = max(info["stops"], key=lambda c: c[0] + c[1] + c[2])
            if sum(color) < 40 and len(info["stops"]) > 1:
                color = avg_colors(info["stops"])
        elif info["href"]:
            color = resolve(info["href"], stack)
        else:
            color = (128, 128, 128)
        memo[gid] = color
        return color

    return {gid: to_hex(resolve(gid)) for gid in raw}


def fix_path(tag: str, gradients: dict[str, str]) -> str:
    tag = re.sub(r'\sstyle="[^"]*"', "", tag, flags=re.IGNORECASE)
    tag = re.sub(r'\saria-label="[^"]*"', "", tag, flags=re.IGNORECASE)

    def repl_url(m: re.Match[str]) -> str:
        gid = m.group(1)
        return f'fill="{gradients.get(gid, "#808080")}"'

    tag = URL_FILL_RE.sub(repl_url, tag)

    has_stroke = re.search(r"\bstroke\s*=", tag, re.IGNORECASE) is not None
    has_fill = re.search(r"\bfill\s*=", tag, re.IGNORECASE) is not None
    if has_stroke and not has_fill:
        tag = re.sub(r"<path\b", '<path fill="none"', tag, count=1, flags=re.IGNORECASE)

    # Drop hairline black strokes on filled shapes — they dominate at 32x13.
    if re.search(r'\bfill\s*=\s*"#', tag, re.IGNORECASE) and not re.search(
        r'fill\s*=\s*"none"', tag, re.IGNORECASE
    ):
        tag = re.sub(r'\sstroke="[^"]*"', "", tag, flags=re.IGNORECASE)
        tag = re.sub(r'\sstroke-width="[^"]*"', "", tag, flags=re.IGNORECASE)

    return tag


def balance_tags(xml: str) -> None:
    og = len(re.findall(r"<g\b", xml, re.I))
    cg = len(re.findall(r"</g>", xml, re.I))
    os_ = len(re.findall(r"<svg\b", xml, re.I))
    cs = len(re.findall(r"</svg>", xml, re.I))
    if og != cg:
        raise ValueError(f"<g> mismatch open={og} close={cg}")
    if os_ != cs:
        raise ValueError(f"<svg> mismatch open={os_} close={cs}")


def convert(text: str) -> str:
    m = SVG_OPEN_RE.search(text)
    if not m:
        raise ValueError("no <svg>")
    vb_m = VIEWBOX_RE.search(m.group(0))
    if not vb_m:
        raise ValueError("no viewBox")
    view_box = vb_m.group(1).strip()

    gradients = build_gradient_map(text)
    body = text[m.end() :]
    if not re.search(r"</svg>\s*$", body, re.I):
        raise ValueError("missing </svg>")

    # Remove defs after we resolved colors from them.
    body = DEFS_RE.sub("", body, count=1)
    body = PATH_RE.sub(lambda mm: fix_path(mm.group(0), gradients), body)

    header = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<!-- Generator: Adobe Illustrator 16.0.0, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->\n"
        '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" '
        '"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">\n'
        '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px"\n'
        '\t width="32px" height="13px" '
        f'viewBox="{view_box}" '
        'enable-background="new 0 0 32 13" xml:space="preserve">\n'
    )
    out = header + body
    if not out.rstrip().endswith("</svg>"):
        out = out.rstrip() + "\n</svg>\n"
    balance_tags(out)
    if "url(#" in out:
        raise ValueError("still has url(#...) fills after flatten")
    return out if out.endswith("\n") else out + "\n"


def main() -> None:
    sources = sorted(BACKUP.glob("skillgroup*.svg"))
    if not sources:
        raise SystemExit(f"no files in {BACKUP}")

    for bak in sources:
        raw = bak.read_text(encoding="utf-8")
        try:
            out = convert(raw)
        except Exception as e:
            print(f"FAIL {bak.name}: {e}")
            continue
        (OUT / bak.name).write_text(out, encoding="utf-8", newline="\n")
        # quick sanity: solid fills present
        fills = len(re.findall(r'fill="#[0-9A-Fa-f]{6}"', out))
        print(f"OK   {bak.name}  solid_fills={fills}")

    print("\nGradients flattened to solid fills (CS2 cannot render url(#gradient)).")
    print("Copy into content/csgo_addons/<addon>/panorama/images/icons/skillgroups/")
    print("Delete game/csgo_addons/<addon>/.../skillgroups/* first so Tools recompile.")


if __name__ == "__main__":
    main()
