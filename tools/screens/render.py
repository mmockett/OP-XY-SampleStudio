#!/usr/bin/env python3
"""Renders the harness's frames (run.sh) to SVG and PNG, as the SDK's browser renderer applies a
retained frame (sdk/browser/display.mjs SvgDisplay): each mutable path's points, each node's
transform and paint, each text slot. Text in the OP-XY's screen font, Univers Next Pro Light:
set XY_FONT to its file (it is never committed). PNGs need Google Chrome.

    XY_FONT=<font.ttf> python3 render.py <open-xy checkout> <out folder> [scale]
"""
import os
import pathlib
import re
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET

CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'
SVG = 'http://www.w3.org/2000/svg'
ET.register_namespace('', SVG)


def bindings(panel_hpp):
    text = panel_hpp.read_text()
    counts = dict(re.findall(r'k(Points|Paths|Nodes|Texts|Palette) = (\d+)', text))
    ids = re.findall(r'"(ss_[a-z0-9_]+)"', text.split('kIds[] = {', 1)[1].split('};', 1)[0])
    n = {k: int(v) for k, v in counts.items()}
    nodes = ids[:n['Nodes']]
    texts = ids[n['Nodes']:n['Nodes'] + n['Texts']]
    palette = ids[n['Nodes'] + n['Texts']:]
    assert len(palette) == n['Palette'], 'panel bindings'
    return n['Paths'], n['Points'], nodes, texts, palette


def decimal(word):
    return str(round(word / 1024 * 1000) / 1000)


def render(svg_text, frame, paths, points, nodes, texts, palette):
    root = ET.fromstring(svg_text)
    by_id = {e.get('id'): e for e in root.iter() if e.get('id')}
    words = struct.unpack('<%di' % (len(frame) // 4), frame)
    stride = 1 + 2 * points
    nodes_at = max(paths, 1) * stride
    texts_at = nodes_at + 7 * len(nodes)
    assert len(words) == texts_at + 9 * len(texts) + 8, 'frame size'
    paint = []
    for pid in palette:
        e = by_id[pid]
        paint.append((e.get('fill'), e.get('stroke'), e.get('stroke-width')))
    for p in range(paths):
        w = words[p * stride + 1:(p + 1) * stride]
        d = ' '.join(('L' if i else 'M') + decimal(w[2 * i]) + ' ' + decimal(w[2 * i + 1]) for i in range(points))
        by_id[nodes[p]].set('d', d + ' Z')
    for i, nid in enumerate(nodes):
        x, y, scale, cos, sin, shade, how = words[nodes_at + 7 * i:nodes_at + 7 * i + 7]
        a, b = (scale * cos) // 1024, (scale * sin) // 1024
        e = by_id[nid]
        e.set('transform', 'matrix(%s %s %s %s %s %s)' % (decimal(a), decimal(b), decimal(-b), decimal(a), decimal(x), decimal(y)))
        if how:
            fill, stroke, width = paint[shade]
            e.set('stroke', stroke)
            e.set('stroke-width', width)
            if how == 2 and (e.get('fill') or 'none') != 'none':
                e.set('fill', fill)
    for i, tid in enumerate(texts):
        x, y, scale = words[texts_at + 9 * i:texts_at + 9 * i + 3]
        raw = frame[(texts_at + 9 * i + 3) * 4:(texts_at + 9 * i + 9) * 4]
        content = raw.split(b'\0', 1)[0].decode()
        e = by_id[tid]
        e.set('transform', 'matrix(%s 0 0 %s %s %s)' % (decimal(scale), decimal(scale), decimal(x), decimal(y)))
        e.text = content
    for pid in palette:
        by_id[pid].set('visibility', 'hidden')   # palette entries lend paint; the device never draws them
    return ET.tostring(root, encoding='unicode')


def main():
    sdk, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    font = os.environ.get('XY_FONT')
    if not font or not pathlib.Path(font).is_file():
        sys.exit('set XY_FONT to UniversNextPro-Light.ttf')
    platform = sdk / 'sdk/platform/opxy-1.2.1'
    paths, points, nodes, texts, palette = bindings(platform / 'sample_studio_panel.hpp')
    svg_text = (platform / 'sample_studio_panel.svg').read_text()
    width, height = 480, 222
    (out / 'svg').mkdir(parents=True, exist_ok=True)
    (out / 'png').mkdir(parents=True, exist_ok=True)
    for frame_path in sorted((out / 'frames').glob('*.bin')):
        svg = render(svg_text, frame_path.read_bytes(), paths, points, nodes, texts, palette)
        svg = svg.replace('font-family="UniversNextPro-Light"', 'font-family="Screen"')
        (out / 'svg' / (frame_path.stem + '.svg')).write_text(svg)
        page = out / 'svg' / (frame_path.stem + '.html')
        page.write_text(
            '<!doctype html><html><head><style>@font-face{font-family:Screen;src:url("file://%s")}'
            'html,body{margin:0;background:#000}svg{display:block;width:%dpx;height:%dpx}</style></head>'
            '<body>%s</body></html>' % (font, width, height,
                                        svg.replace('<svg ', '<svg viewBox="0 0 %d %d" ' % (width, height), 1)
                                           .replace('width="480" height="220" viewBox="0 0 480 220"', '')))
        png = out / 'png' / (frame_path.stem + '.png')
        subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--allow-file-access-from-files', '--hide-scrollbars',
                        '--force-device-scale-factor=%d' % scale, '--window-size=%d,%d' % (width, height),
                        '--virtual-time-budget=2000', '--screenshot=%s' % png, page.as_uri()],
                       check=True, capture_output=True)
        print(png)


if __name__ == '__main__':
    main()
