#!/usr/bin/env python3
"""Redraw Inkscape skillgroup SVGs into CS2/Illustrator-compatible 32x13 SVGs.

CS2 VSVG rejects Inkscape features (relative cmds, group transforms, xlink
gradients, mm sizes). This bakes geometry via svgelements and emits absolute
M/C/L/Z paths with solid fills inside viewBox 0 0 32 13.
"""
from __future__ import annotations

import re
import xml.etree.ElementTree as ET
from pathlib import Path

from svgelements import SVG, Path as SvgPath

OUT = Path(__file__).resolve().parent
BACKUP = OUT / "_backup_before_32x13"

NS = {
    "svg": "http://www.w3.org/2000/svg",
    "xlink": "http://www.w3.org/1999/xlink",
}
# ElementTree leaves tags as {uri}local when namespaced
TAG = re.compile(r"\{.*\}(.*)$")


def local(tag: str) -> str:
    m = TAG.match(tag)
    return m.group(1) if m else tag


def parse_hex(color: str | None) -> tuple[int, int, int] | None:
    if not color:
        return None
    color = color.strip().lower()
    if color in ("white", "#fff", "#ffffff"):
        return (255, 255, 255)
    if color in ("black", "#000", "#000000"):
        return (0, 0, 0)
    if color.startswith("#") and len(color) == 4:
        color = "#" + "".join(c * 2 for c in color[1:])
    if color.startswith("#") and len(color) == 7:
        return int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16)
    return None


def to_hex(rgb: tuple[int, int, int]) -> str:
    return f"#{rgb[0]:02X}{rgb[1]:02X}{rgb[2]:02X}"


def brighter(colors: list[tuple[int, int, int]]) -> tuple[int, int, int]:
    if not colors:
        return (128, 128, 128)
    return max(colors, key=lambda c: c[0] + c[1] + c[2])


def gradient_solids(xml_text: str) -> dict[str, str]:
    """Resolve Inkscape linearGradient (+ xlink:href) to solid #RRGGBB."""
    root = ET.fromstring(xml_text)
    raw: dict[str, dict] = {}
    for el in root.iter():
        if local(el.tag) != "linearGradient":
            continue
        gid = el.attrib.get("id")
        if not gid:
            continue
        href = el.attrib.get("{http://www.w3.org/1999/xlink}href") or el.attrib.get("href")
        stops: list[tuple[int, int, int]] = []
        for stop in el:
            if local(stop.tag) != "stop":
                continue
            rgb = parse_hex(stop.attrib.get("stop-color"))
            if rgb is None and "stop-color" not in stop.attrib:
                # bare <stop> defaults to black
                style = stop.attrib.get("style", "")
                sm = re.search(r"stop-color\s*:\s*([^;]+)", style)
                rgb = parse_hex(sm.group(1) if sm else None) or (0, 0, 0)
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
            color = brighter(info["stops"])
            if sum(color) < 40 and len(info["stops"]) > 1:
                n = len(info["stops"])
                color = (
                    sum(c[0] for c in info["stops"]) // n,
                    sum(c[1] for c in info["stops"]) // n,
                    sum(c[2] for c in info["stops"]) // n,
                )
        elif info["href"]:
            color = resolve(info["href"], stack)
        else:
            color = (128, 128, 128)
        memo[gid] = color
        return color

    return {gid: to_hex(resolve(gid)) for gid in raw}


def resolve_fill(fill_attr: str | None, gradients: dict[str, str]) -> str | None:
    if not fill_attr or fill_attr == "none":
        return None
    m = re.match(r"url\(#([^)]+)\)", fill_attr.strip())
    if m:
        return gradients.get(m.group(1), "#808080")
    rgb = parse_hex(fill_attr)
    return to_hex(rgb) if rgb else None


def fmt_num(v: float) -> str:
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return s if s else "0"


def path_to_absolute_d(path: SvgPath, sx: float, sy: float) -> str:
    """Emit absolute path commands scaled into 32x13 space."""
    # Force absolute segment list
    d = path.d(relative=False)
    # Scale coordinates in the d string numerically via segment iteration
    parts: list[str] = []
    for seg in path.segments():
        name = type(seg).__name__
        if name == "Move":
            parts.append(f"M{fmt_num(seg.end.x * sx)},{fmt_num(seg.end.y * sy)}")
        elif name == "Close":
            parts.append("Z")
        elif name == "Line":
            parts.append(f"L{fmt_num(seg.end.x * sx)},{fmt_num(seg.end.y * sy)}")
        elif name == "CubicBezier":
            parts.append(
                "C"
                f"{fmt_num(seg.control1.x * sx)},{fmt_num(seg.control1.y * sy)} "
                f"{fmt_num(seg.control2.x * sx)},{fmt_num(seg.control2.y * sy)} "
                f"{fmt_num(seg.end.x * sx)},{fmt_num(seg.end.y * sy)}"
            )
        elif name == "QuadraticBezier":
            # Convert Q to cubic for broader CS2 support
            # CP1 = start + 2/3*(qcp-start), CP2 = end + 2/3*(qcp-end)
            sx0, sy0 = seg.start.x, seg.start.y
            cx, cy = seg.control.x, seg.control.y
            ex, ey = seg.end.x, seg.end.y
            c1x = sx0 + 2 / 3 * (cx - sx0)
            c1y = sy0 + 2 / 3 * (cy - sy0)
            c2x = ex + 2 / 3 * (cx - ex)
            c2y = ey + 2 / 3 * (cy - ey)
            parts.append(
                "C"
                f"{fmt_num(c1x * sx)},{fmt_num(c1y * sy)} "
                f"{fmt_num(c2x * sx)},{fmt_num(c2y * sy)} "
                f"{fmt_num(ex * sx)},{fmt_num(ey * sy)}"
            )
        elif name == "Arc":
            # Approximate arc as cubics already done by as_cubic_curves if available
            try:
                for c in seg.as_cubic_curves():
                    parts.append(
                        "C"
                        f"{fmt_num(c.control1.x * sx)},{fmt_num(c.control1.y * sy)} "
                        f"{fmt_num(c.control2.x * sx)},{fmt_num(c.control2.y * sy)} "
                        f"{fmt_num(c.end.x * sx)},{fmt_num(c.end.y * sy)}"
                    )
            except Exception:
                parts.append(f"L{fmt_num(seg.end.x * sx)},{fmt_num(seg.end.y * sy)}")
        else:
            # fallback
            if hasattr(seg, "end"):
                parts.append(f"L{fmt_num(seg.end.x * sx)},{fmt_num(seg.end.y * sy)}")
    return "".join(parts)


def redraw(src: Path) -> str:
    xml_text = src.read_text(encoding="utf-8")
    gradients = gradient_solids(xml_text)

    svg = SVG.parse(str(src))
    w = float(svg.width)
    h = float(svg.height)
    if w <= 0 or h <= 0:
        raise ValueError(f"bad size {w}x{h}")
    sx = 32.0 / w
    sy = 13.0 / h

    paths_out: list[tuple[str, str]] = []  # (fill, d)

    for el in svg.elements():
        if type(el).__name__ != "Path":
            continue
        vals = getattr(el, "values", {}) or {}
        fill_attr = vals.get("fill")
        # Stroke-only outline with implicit black fill in Inkscape source:
        # original outer path has stroke but no fill → skip (black silhouette).
        if fill_attr is None or fill_attr == "none":
            continue

        fill = resolve_fill(fill_attr, gradients)
        if not fill:
            continue

        try:
            el.reify()
            path = SvgPath(el)
        except Exception:
            continue
        if not path or path.bbox() is None:
            continue

        d = path_to_absolute_d(path, sx, sy)
        if not d or d == "Z":
            continue
        # Drop near-invisible dark fills that were stroke silhouettes
        rgb = parse_hex(fill)
        if rgb and sum(rgb) < 25:
            continue
        paths_out.append((fill, d))

    if not paths_out:
        raise ValueError("no drawable paths")

    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generator: Adobe Illustrator 16.0.0, SVG Export Plug-In . SVG Version: 6.00 Build 0)  -->",
        '<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">',
        '<svg version="1.1" id="Layer_1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" x="0px" y="0px"',
        '\t width="32px" height="13px" viewBox="0 0 32 13" enable-background="new 0 0 32 13" xml:space="preserve">',
    ]
    for fill, d in paths_out:
        lines.append(f'\t<path fill="{fill}" d="{d}"/>')
    lines.append("</svg>")
    return "\n".join(lines) + "\n"


def main() -> None:
    sources = sorted(BACKUP.glob("skillgroup*.svg"))
    if not sources:
        raise SystemExit(f"no files in {BACKUP}")
    for bak in sources:
        try:
            out = redraw(bak)
        except Exception as e:
            print(f"FAIL {bak.name}: {e}")
            continue
        (OUT / bak.name).write_text(out, encoding="utf-8", newline="\n")
        n = out.count("<path ")
        print(f"OK   {bak.name}  paths={n}  bytes={len(out)}")


if __name__ == "__main__":
    main()
