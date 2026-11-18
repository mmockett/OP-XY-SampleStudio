"""Sample Studio's page artwork.

Writes, into the extension (sample_studio/):
  sample_studio.svg   installed as content/display/sample_studio.svg
  panel.hpp           its node and text IDs, layout constants and
                                                    the screen font's advance widths
Every element the page draws is a node or text slot the frame places, so the
same document draws the whole page or only the library's "edit" label.

    XY_FONT=.../UniversNextPro-Light.ttf python3 -I tools/panel.py

The advance widths are read from the device font (never committed): a table
of 95 numbers, so the device can centre a label over its key as stock does.
"""
import os
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "sample_studio"
POINTS = 64

# Mutable paths first (the frame reshapes them), in this order.
# A waveform quarter is two thin ribbons, its top edge (dim, lit) and its bottom edge
# (dimlo, litlo), so no outline closes across the waveform where two quarters meet and
# no dents show between them at any zoom.
PATHS = (["dim0", "dim1", "dim2", "dim3", "lit0", "lit1", "lit2", "lit3"] +
         [f"dimlo{i}" for i in range(4)] + [f"litlo{i}" for i in range(4)] +
         ["strip0", "strip1", "bracket", "progress"] +
         # the render's progress (red) and its finish (green), filled copies of the
         # waveform drawn beneath its outline
         [f"red{i}" for i in range(4)] + [f"green{i}" for i in range(4)])
# Fixed outlines the frame only moves.
FIXED = (["bg", "bar0", "bar1", "bar2", "bar3", "bar4", "bar5", "bar6", "bar7", "start", "end",
          "tab0", "tab1", "tab2", "tab3", "card", "track", "playhead", "zero0", "zero1"] + [f"cut{i}" for i in range(23)])
PAGES = ["trim", "slice", "retime", "process"]   # M1..M4
# The file line's alert text ("about to render", "rendering", "loading the render"): red, as stock
# marks recording. Text colour comes from the artwork, so this slot is authored red (assumed value).
RED = "#FF3020"
# Text slots: (name, px, fill)
TEXTS = ([(f"title{i}", 10, "#CBCBCB" if i < 2 else "#000000") for i in range(4)] +
         [(f"value{i}", 20, "#CBCBCB" if i < 2 else "#000000") for i in range(4)] +
         [(f"under_title{i}", 10, "#5E5E5E") for i in range(4)] +
         [(f"under_value{i}", 14, "#9A9A9A") for i in range(4)] +
         [("file_left", 10, "#9A9A9A"), ("file_right", 10, "#5E5E5E"), ("file_alert", 10, RED)] +
         [(f"m{i}", 20, "#D8D8D8") for i in range(4)] +
         [("tab_text", 20, "#000000"), ("card1", 20, "#000000"), ("card2", 14, "#000000")])
BAR = ["#000000", "#323232", "#4B4B4B", "#646464", "#7D7D7D", "#969696", "#AFAFAF", "#CDCDCD"]
PARK = "matrix(1 0 0 1 640 320)"
# Palette: entry 0 lends white; 1..4 are the finish flash's greens, bright to dark, which the
# filled green copy steps through before it hides (its fill is authored, so the palette recolours it).
PALETTE = ["#FFFFFF", "#38E070", "#249A4C", "#14582C", "#082612"]


def ribbon(n, width, height):
    """A closed outline of n points: a rectangle with points spread along its
    edges, never closer than width / n, so the device can reshape it."""
    top = n // 2
    pts = [(-width / 2 + width * i / (top - 1), -height / 2) for i in range(top)]
    pts += [(width / 2 - width * i / (n - top - 1), height / 2) for i in range(n - top)]
    return pts


def path_d(pts):
    return "M" + " L".join(f"{x:.3f},{y:.3f}" for x, y in pts) + " Z"


def rect(x, y, w, h):
    return f"M{x},{y} L{x + w},{y} L{x + w},{y + h} L{x},{y + h} Z"


def advances(font):
    data = pathlib.Path(font).read_bytes()
    tables = {}
    count = struct.unpack(">H", data[4:6])[0]
    for i in range(count):
        tag, _, offset, length = struct.unpack(">4sIII", data[12 + 16 * i:28 + 16 * i])
        tables[tag.decode()] = (offset, length)
    units = struct.unpack(">H", data[tables["head"][0] + 18:tables["head"][0] + 20])[0]
    metrics = struct.unpack(">H", data[tables["hhea"][0] + 34:tables["hhea"][0] + 36])[0]
    cmap_offset = tables["cmap"][0]
    glyph = {}
    for i in range(struct.unpack(">H", data[cmap_offset + 2:cmap_offset + 4])[0]):
        platform, encoding, sub = struct.unpack(">HHI", data[cmap_offset + 4 + 8 * i:cmap_offset + 12 + 8 * i])
        at = cmap_offset + sub
        if struct.unpack(">H", data[at:at + 2])[0] != 4 or (platform, encoding) not in ((3, 1), (0, 3)):
            continue
        segs = struct.unpack(">H", data[at + 6:at + 8])[0] // 2
        ends = struct.unpack(f">{segs}H", data[at + 14:at + 14 + 2 * segs])
        starts = struct.unpack(f">{segs}H", data[at + 16 + 2 * segs:at + 16 + 4 * segs])
        deltas = struct.unpack(f">{segs}h", data[at + 16 + 4 * segs:at + 16 + 6 * segs])
        ranges_at = at + 16 + 6 * segs
        ranges = struct.unpack(f">{segs}H", data[ranges_at:ranges_at + 2 * segs])
        for s in range(segs):
            for c in range(starts[s], min(ends[s], 126) + 1):
                if c < 32:
                    continue
                if ranges[s] == 0:
                    glyph[c] = (c + deltas[s]) & 0xffff
                else:
                    g_at = ranges_at + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", data[g_at:g_at + 2])[0]
                    glyph[c] = (g + deltas[s]) & 0xffff if g else 0
        break
    hmtx = tables["hmtx"][0]
    out = []
    for c in range(32, 127):
        g = glyph.get(c, 0)
        g = min(g, metrics - 1)
        adv = struct.unpack(">H", data[hmtx + 4 * g:hmtx + 4 * g + 2])[0]
        out.append(round(adv * 1000 / units))
    return out


TAB_WIDTHS = [44, 52, 46, 54]   # each page's label at 20 px and 4 px either side; set from the font in main()


def svg():
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="480" height="220" viewBox="0 0 480 220">',
             '<g id="sample_studio_panel">']

    def node(name, d, fill, stroke, sw):
        parts.append(f'<path id="ss_{name}" transform="{PARK}" d="{d}" fill="{fill}" stroke="{stroke}" '
                     f'stroke-width="{sw}"/>')

    node("bg", rect(0, 0, 480, 220), "#000000", "none", 0)
    for i in range(8):
        node(f"bar{i}", rect(0, 0, 60, 20), BAR[i], BAR[i], 0.5)
    for i in range(4):
        node(f"red{i}", path_d(ribbon(POINTS, 100, 4)), RED, RED, 0.3)
    for i in range(4):
        node(f"green{i}", path_d(ribbon(POINTS, 100, 4)), PALETTE[1], PALETTE[1], 0.3)
    for i in range(4):
        node(f"dim{i}", path_d(ribbon(POINTS, 100, 4)), "none", "#5E5E5E", 1)
    for i in range(4):
        node(f"lit{i}", path_d(ribbon(POINTS, 100, 4)), "none", "#D8D8D8", 1)
    for i in range(4):
        node(f"dimlo{i}", path_d(ribbon(POINTS, 100, 4)), "none", "#5E5E5E", 1)
    for i in range(4):
        node(f"litlo{i}", path_d(ribbon(POINTS, 100, 4)), "none", "#D8D8D8", 1)
    for i in range(2):
        node(f"strip{i}", path_d(ribbon(POINTS, 100, 4)), "none", "#5E5E5E", 0.6)
    node("bracket", path_d(ribbon(POINTS, 60, 12)), "none", "#FFFFFF", 1)
    for i in range(23):
        node(f"cut{i}", "M-0.3,-54 L0.3,-54 L0.3,54 L-0.3,54 Z", "#9A9A9A", "#9A9A9A", 0.3)
    marker = "M-0.6,-56 L0.6,-56 L0.6,56 L3,56 L3,62 L-3,62 L-3,56 L-0.6,56 Z"
    # the playhead: a 1 px line over the stage
    node("playhead", "M-0.5,-54 L0.5,-54 L0.5,54 L-0.5,54 Z", "#FFFFFF", "#FFFFFF", 0.1)
    node("start", marker, "#FFFFFF", "#FFFFFF", 0.3)
    node("end", marker, "#FFFFFF", "#FFFFFF", 0.3)
    # a marker on a zero crossing (snap): a small diamond over its top
    for i in range(2):
        node(f"zero{i}", "M0,-63 L2.5,-60 L0,-57 L-2.5,-60 Z", "#FFFFFF", "#FFFFFF", 0.2)
    for i in range(4):
        node(f"tab{i}", rect(0, 0, TAB_WIDTHS[i], 23), "#D8D8D8", "#D8D8D8", 0.5)
    node("card", rect(-160, -35, 320, 70), "#CDCDCD", "#CDCDCD", 0.5)
    node("track", rect(-64, -1, 128, 2), "#7A7A7A", "#7A7A7A", 0.3)
    node("progress", path_d(ribbon(POINTS, 100, 2)), "#000000", "#000000", 0.3)
    for name, px, fill in TEXTS:
        parts.append(f'<text id="ss_{name}" transform="{PARK}" fill="{fill}" font-family="UniversNextPro-Light" '
                     f'font-size="{px}px"> </text>')
    for i, colour in enumerate(PALETTE):
        name = "ss_palette" if i == 0 else f"ss_palette{i}"
        parts.append(f'<path id="{name}" transform="matrix(1 0 0 1 640 320)" d="M0,0 L1,0 L1,1 Z" fill="{colour}" '
                     f'stroke="{colour}" stroke-width="0.3"/>')
    parts.append("</g></svg>")
    return "\n".join(parts) + "\n"


def header(widths):
    ids = ([f"ss_{n}" for n in PATHS + FIXED] + [f"ss_{n}" for n, _, _ in TEXTS] +
           ["ss_palette" if i == 0 else f"ss_palette{i}" for i in range(len(PALETTE))])
    lines = ["// Generated by tools/panel.py: Sample Studio's page artwork binding.",
             "#pragma once", '#include <opxy/draw.hpp>', "",
             "namespace opxy { namespace extensions { namespace sample_studio { namespace studio_panel {",
             f"const u32 kPoints = {POINTS};",
             f"const u32 kPaths = {len(PATHS)}, kNodes = {len(PATHS) + len(FIXED)}, kTexts = {len(TEXTS)}, kPalette = {len(PALETTE)};"]
    for i, n in enumerate(PATHS + FIXED):
        lines.append(f"const u32 N_{n.upper()} = {i};")
    for i, (n, px, _) in enumerate(TEXTS):
        lines.append(f"const u32 T_{n.upper()} = {i};  // {px} px")
    lines.append("using Frame = draw::Frame<kPaths, kPoints, kNodes, kTexts, kPalette>;")
    lines.append("const char* const kIds[] = {")
    lines += [f'    "{i}",' for i in ids]
    lines.append("};")
    lines.append("// Advance widths of ' '..'~' in the screen font, thousandths of an em.")
    lines.append("const unsigned short kAdvance[95] = {" + ", ".join(str(w) for w in widths) + "};")
    lines.append("const i32 kTabWidth[4] = {" + ", ".join(str(w) for w in TAB_WIDTHS) + "};")
    lines.append('const char kAsset[] = "sample_studio.svg";')
    lines.append("}}}}")
    return "\n".join(lines) + "\n"


def main():
    font = os.environ.get("XY_FONT")
    if not font or not pathlib.Path(font).is_file():
        sys.exit("set XY_FONT to UniversNextPro-Light.ttf")
    widths = advances(font)
    for i, page in enumerate(PAGES):
        TAB_WIDTHS[i] = round(sum(widths[ord(c) - 32] for c in page) * 20 / 1000) + 8
    (OUT / "sample_studio.svg").write_text(svg(), encoding="utf-8")
    (OUT / "panel.hpp").write_text(header(widths), encoding="utf-8")


if __name__ == "__main__":
    main()
