"""Sample Studio, the extension (sample_studio/): its editor, player and render chain.

Pure Python: no host compile. It checks that the editor and player use only the SDK's
services (opxy/sample_studio.hpp), that the artwork binds the IDs the C++ names, that every
assumption is listed once, and that the render chain, transcribed with 32-bit arithmetic,
matches the exact Python reference here (studio_ref.py, studio_chain.py) sample for sample.
The SDK's side (its sites, services and tooling) is checked against an Open-XY checkout with
Sample Studio's capability: OPEN_XY, or ../open-xy beside this repository, as CI has it.
"""
import base64
import json
import math
import os
import random
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXT = REPO / 'sample_studio'
ROOT = Path(os.environ.get('OPEN_XY', REPO.parent / 'open-xy')).resolve()   # the SDK checkout
sys.path.insert(0, str(ROOT))

PLATFORM = ROOT / 'sdk/platform/opxy-1.2.1'
STUDIO = EXT / 'editor.cpp'
PLAYER = EXT / 'player.cpp'
HOOKS = PLATFORM / 'sample_studio_hooks.s'
PANEL_SVG = EXT / 'sample_studio.svg'
PANEL_HPP = EXT / 'panel.hpp'
HEADER = EXT / 'bake.hpp'
API = ROOT / 'sdk/include/opxy/sample_studio.hpp'
HOST = PLATFORM / 'sample_studio_host.cpp'
AUDIO = PLATFORM / 'sample_studio_audio.cpp'
REFERENCE = REPO / 'tests'   # the exact Python reference of the render chain
# The five sites: address, the stock bytes, the entry and its trampoline.
SITES = {
    '0x8c0a330a': ('083200e80000f505', 'sdk_sample_studio_press', 'sdk_sample_studio_stock_press'),
    '0x8c0a2be4': ('00e80000f405a66f', 'sdk_sample_studio_shift_press', 'sdk_sample_studio_stock_shift_press'),
    '0x8c0a2d68': ('00e80700b8617005', 'sdk_sample_studio_release', 'sdk_sample_studio_stock_release'),
    '0x8c0a24d0': ('00e8000060058064', 'sdk_sample_studio_turn', 'sdk_sample_studio_stock_turn'),
    '0x8d55341c': ('00e80e00e305666f', 'sdk_sample_studio_frame', 'sdk_sample_studio_stock_frame'),
}
# Stock functions the studio calls, each already used by the SDK, bounce phase 1,
# the library's completion or the slicer.
KNOWN = {'8fed0348', '8fed0780', '8fed00ae', '8c040bd8', '8c040c24', '8c042efc', '8c03e6f4', '8c048788',
         '8c03e9c8', '8d4d2910', '8d549168', '8d4dd728', '8d589370', '8d5be3a0', '8d58938c', '8d5893bc',
         '8d589ac0', '8d589b8c', '8d520ce0', '8d5fe884', '8d51fcd4', '8c0867f0', '8c0bd5bc',
         # A14: stock acquire's own lock, unlock and map lookup (8d509d70 calls all three)
         '8d5be032', '8d5be060', '8d509824',
         # A19: the per-track note-start serial the SDK's screens read (screen.hpp note_serial)
         '8d4d6c08',
         # A23: newlib fseek and ftell (stock seeks a file to its end and asks its size at 8d5cab8e..8d5cabaa)
         '8fed06cc', '8fed0764',
         # A28: stock's tree, its highlighted path, the audition's stop (sample-studio-re-browsers.md)
         '8c0ad5dc', '8c0a84b4', '8c0a1454', '8d4d2910',
         # A19: stock's B3 parameter send, as the master processor's main-board side uses it
         '8d4dc84c',
         # A19's stream: newlib fread, as aux_tracks.cpp and project_companion.cpp use it
         '8fed05bc',
         # A35: the library's change flag, as stock's preset save clears it (8c0b4fd2)
         '8c0a7e70',
         # A37: stock's preset index build, as its preset rename and delete call it (8d4ef74c, 8d4ef670)
         '8d4eefec'}


def scale_frames(a, n, length):
    """editor.cpp scale_frames, with each 32-bit product checked."""
    if not length or n == length:
        return a
    if a >= length:
        return n
    if length < 65536:
        assert a << 16 < 1 << 32
        q16 = (a << 16) // length
    elif a < (1 << 20):
        q16 = (a << 12) // (length >> 4)
    else:
        assert a << 8 < 1 << 32
        q16 = (a << 8) // (length >> 8)
    q16 = min(q16, 65536)
    assert (n >> 16) * q16 < 1 << 32 and (n & 0xFFFF) * q16 < 1 << 32
    out = (n >> 16) * q16 + (((n & 0xFFFF) * q16) >> 16)
    return min(out, n)


class Studio:
    """An extension that declares sample_studio, as extension_at reads one."""
    sample_studio, master_processor, drumline_page, capabilities, linked = True, None, None, (), False


class Built(list):
    """A machine list with its extensions, as a collection hands native.py one."""
    def __init__(self, machines, extensions=()):
        super().__init__(machines)
        self.extensions = tuple(extensions)


class OptIn(unittest.TestCase):
    def test_profile_service_and_its_five_entry_branches(self):
        profile = json.loads((ROOT / 'profiles/opxy-1.2.1.json').read_text())
        self.assertEqual(profile['services']['sample_studio'], ['sampleStudio'])
        self.assertNotIn('sample_probe', profile['services'])
        mine = [p for p in profile['patches'] if p.get('service') == 'sample_studio']
        self.assertEqual({p['address']: (p['before'], p['symbol']) for p in mine},
                         {a: (b, s) for a, (b, s, _) in SITES.items()})
        for p in mine:
            self.assertEqual((p['kind'], p['length'], p['call']), ('hook', 8, False))

    def test_service_user_from_the_extension(self):
        from tooling import native

        class Machine:
            kind = 'sample'
            definition = {}

        class Synth:
            kind = 'synth'
            definition = {}
        self.assertNotIn('sampleStudio', native.service_users([Machine()]))
        self.assertNotIn('sampleStudio', native.service_users(Built([Machine()])))
        self.assertIn('sampleStudio', native.service_users(Built([Machine()], [Studio()])))
        with self.assertRaises(Exception):
            native.service_users(Built([Synth()], [Studio()]))   # needs the samples service
        self.assertNotIn('OPXY_SAMPLE_STUDIO', (ROOT / 'tooling/native.py').read_text())

    def test_the_extension_field(self):
        import shutil
        import tempfile
        from tooling import extensions
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder) / 'sample_studio'
            shutil.copytree(EXT, root)
            data = {'id': 'sample_studio', 'name': 'Sample Studio', 'description': 'Edit samples on the OP-XY.',
                    'claims': [], 'actions': [], 'sample_studio': {}}
            (root / 'extension.json').write_text(json.dumps(data))
            e = extensions.extension_at(root)
            self.assertTrue(e.sample_studio)
            self.assertTrue(e.linked)                                          # the editor and player are its code
            row = extensions.metadata([e], {'sample_studio': 'personal'})[0]['sample_studio']
            self.assertEqual(row['asset'], 'sample_studio.svg')                 # the builder installs it
            self.assertEqual(base64.b64decode(row['artwork']), PANEL_SVG.read_bytes())
            (root / 'extension.json').write_text(json.dumps(dict(data, sample_studio={'x': 1})))
            with self.assertRaises(Exception):
                extensions.extension_at(root)                                  # no options yet
            (root / 'extension.json').write_text(json.dumps({k: v for k, v in data.items() if k != 'sample_studio'}))
            self.assertFalse(extensions.extension_at(root).sample_studio)     # code alone doesn't claim the library
            (root / 'extension.json').write_text(json.dumps(data))
            (root / 'sample_studio.svg').unlink()
            with self.assertRaises(Exception):
                extensions.extension_at(root)                                  # the artwork comes with it
        text = (ROOT / 'tooling/extensions.py').read_text()
        self.assertIn("require(len(studios) <= 1, 'Sample Studio has multiple owners: '", text)
        self.assertIn("require(not (studios and processors), 'Sample Studio and a master processor share hook sites", text)

    def test_units_belong_to_the_service(self):
        from tooling import native
        for unit in native.SAMPLE_STUDIO_UNITS:
            self.assertEqual(native.UNIT_SERVICES[unit], 'sample_studio')
            self.assertTrue((PLATFORM / (unit + ('.s' if unit.endswith('hooks') else '.cpp'))).is_file())
        self.assertFalse((PLATFORM / 'sample_studio_label_on.cpp').exists())   # the editor is the extension's
        self.assertFalse((PLATFORM / 'sample_studio.cpp').exists())

    def test_no_key_observer_left(self):
        self.assertNotIn('sdk_key_observe', (PLATFORM / 'extension_stock.cpp').read_text())
        self.assertNotIn('sdk_key_observe', (ROOT / 'tooling/startup.py').read_text())
        self.assertFalse((PLATFORM / 'sample_probe.cpp').exists())


class Stubs(unittest.TestCase):
    def test_each_trampoline_replays_its_bytes_and_returns_after_them(self):
        text = HOOKS.read_text()
        for address, (before, _symbol, stub) in SITES.items():
            comment = text.split(f'.global {stub}\n', 1)[0].rsplit('/*', 1)[1]
            body = text.split(f'{stub}:\n', 1)[1].split('.size', 1)[0]
            self.assertIn(address[2:], comment)
            replayed = ''.join(re.findall(r'[0-9a-f]{4}', comment.rsplit('(', 1)[1].split(')')[0].replace(' ', '')))
            self.assertEqual(replayed, before, stub)
            target = int(address, 16) + 8
            self.assertIn(f'P0.H = 0x{target >> 16:04x}; P0.L = 0x{target & 0xffff:04x};', body)
            self.assertIn('JUMP (P0);', body)

    def test_the_sdk_defines_every_entry_and_the_extension_its_callbacks(self):
        host = HOST.read_text()
        for _a, (_b, symbol, stub) in SITES.items():
            self.assertRegex(host, rf'\n(u32|void) {symbol}\(')
            self.assertIn(f'__asm__("{stub}")', host)
        extension = (EXT / 'extension.cpp').read_text()
        self.assertIn('extern const studio::Editor editor = {edit::press, edit::release_key, edit::turn_encoder, edit::frame};', extension)
        self.assertIn('play::receive, play::mixer_begin, play::voice, play::gain, play::level, play::master', extension)


class Panel(unittest.TestCase):
    def test_artwork_binds_every_id_and_mutable_paths_have_64_points(self):
        header = PANEL_HPP.read_text()
        ids = re.findall(r'^    "(ss_[a-z_0-9]+)",$', header, re.MULTILINE)
        counts = dict(re.findall(r'k(Paths|Nodes|Texts) = (\d+)', header))
        paths, nodes, texts = int(counts['Paths']), int(counts['Nodes']), int(counts['Texts'])
        palette = int(re.search(r'kPalette = (\d+)', header).group(1))
        self.assertEqual(len(ids), nodes + texts + palette)
        tree = ET.fromstring(PANEL_SVG.read_text())
        by_id = {e.get('id'): e for e in tree.iter() if e.get('id')}
        for i, name in enumerate(ids):
            self.assertIn(name, by_id)
            tag = by_id[name].tag.split('}')[-1]
            self.assertEqual(tag, 'text' if nodes <= i < nodes + texts else 'path', name)
        for name in ids[:paths]:
            d = by_id[name].get('d')
            self.assertEqual((d.count('M'), d.count('L'), d.endswith('Z')), (1, 63, True), name)
            want = ('#000000' if 'progress' in name else '#FF3020' if 'red' in name else
                    '#38E070' if 'green' in name else 'none')
            self.assertEqual(by_id[name].get('fill'), want, name)
        # the slice lines sit beneath the card, so a card covers them
        order = [e.get('id') for e in tree.iter() if e.get('id')]
        self.assertLess(order.index('ss_cut22'), order.index('ss_card'))
        self.assertEqual(len(re.search(r'kAdvance\[95\] = \{([^}]*)\}', header).group(1).split(',')), 95)

    def test_asset_installed_only_with_the_extension(self):
        from unittest import mock
        from tooling import native
        studio = Studio()
        studio.root = EXT
        studio.id = 'sample_studio'
        profile = {'stock_catalog_sha256': native.sha(b'{}')}
        with mock.patch.object(native, 'extend_catalog', lambda *a: b'{}'), \
                mock.patch.object(native.stock_content, 'check_output', lambda *a: None):
            self.assertNotIn('content/display/sample_studio.svg', native.prepare_assets(b'{}', profile, Built([])))
            outputs = native.prepare_assets(b'{}', profile, Built([], [studio]))
            self.assertEqual(outputs['content/display/sample_studio.svg'], PANEL_SVG.read_bytes())


class Source(unittest.TestCase):
    def test_assumptions_listed_once(self):
        text = STUDIO.read_text()
        for n in range(1, 25):
            self.assertEqual(len(re.findall(rf'^// A{n} ', text, re.MULTILINE)), 1, f'A{n}')

    def test_stock_calls_are_known(self):
        # the extension calls no firmware: every stock call is the SDK's (sample_studio_host.cpp, _audio.cpp)
        for path in (STUDIO, PLAYER, EXT / 'studio_dsp.hpp', HEADER, EXT / 'extension.cpp'):
            code = re.sub(r'/\*.*?\*/|//[^\n]*', '', path.read_text(), flags=re.S)
            self.assertNotIn('stock<', code, path.name)
            self.assertNotIn('__asm__', code, path.name)
            self.assertNotRegex(code, r'0x(8[0-9a-f]|1[19]|0[89])[0-9a-f]{6}\b(?!u?\))' if False else r'reinterpret_cast<[^>]*>\(0x', path.name)
        text = HOST.read_text() + AUDIO.read_text()
        called = set(re.findall(r'\(0x(8[cdf][0-9a-f]{6})\)', text))
        self.assertTrue(called)
        self.assertLessEqual(called, KNOWN | HOST_KNOWN)


# Stock functions the SDK's host calls beyond the studio's first ones (KNOWN): the audio board's
# loader, pool and cache, the master EQ and B3 setter it forwards to, the library's own calls.
HOST_KNOWN = {'8d509608', '8d509df8', '8c048a26', '8d50eb8c', '8c0a7e70', '8c0a349c', '8d4eefec', '8d58938c',
              '8d5893bc', '8d589ac0', '8d589b8c', '8d520ce0', '8d5fe884', '8c0a84b4', '8c0a1454', '8d51fcd4', '8c0867f0',
              '8d4d6c08', '8d5be3a0', '8c042efc', '8c03e6f4', '8c03e9c8', '8c048788', '8d4dd728', '8fed0348',
              '8fed05bc', '8fed0780', '8fed00ae', '8c040bd8', '8c040c24', '8c0bd5bc', '8d549168', '8d589370',
              '8d509824', '8d5be032', '8d5be060', '8d4dc84c', '8d4d2910', '8c0ad5dc'}


# ---------------------------------------------------------------- arithmetic
I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
Q28 = 1 << 28


def wrap(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


def sat(v):
    return I32_MIN if v < I32_MIN else I32_MAX if v > I32_MAX else v


def gain_word(x, gain):
    high = wrap(x * (gain >> 16))
    low = wrap(x * (gain & 0xffff))
    if high > 0x7ffffff:
        return I32_MAX
    if high < -0x8000000:
        return I32_MIN
    return sat(wrap(high * 16) + (low >> 12))


def mul31(a, b):
    return sat((a * b) >> 31)


def dc_a(rate):
    """DcBlock::start, transcribed."""
    k_hi, k_lo = 31, 1786390869
    low = (k_lo + (rate >> 1)) & 0xffffffff
    high = k_hi + (1 if low < k_lo else 0)
    return (1 << 31) - ((high << 32) | low) // rate


def header_tables():
    text = HEADER.read_text()

    def table(name):
        body = re.search(name + r' = \{([^}]*)\}', text, re.S).group(1)
        return [int(v.strip().rstrip('u')) for v in body.replace('\n', ' ').split(',')]
    return table(r'kLevelGain\[51\]'), table(r'kNormalTarget\[4\]'), table(r'kSilenceThreshold\[7\]')


def device_bake(R, frames16, rate, channels, begin, end, reverse, level, normal, dc, fade_in, fade_out, chans):
    """editor.cpp first_job, chain and write_slice, transcribed."""
    gains, targets, _ = header_tables()
    n = end - begin
    if channels < 2:
        chans = 0
    out_channels = channels if chans == 0 else 1
    if normal:
        peak = max(abs(v) for row in frames16[begin:end] for v in row)
        target = targets[normal - 1]
        gain = Q28 if not peak else 0xffffffff if peak * 16 <= target else (target << 28) // peak
    else:
        gain = gains[level + 30]
    fi = min(n, rate * fade_in // 1000) if fade_in else 0
    fo = min(n, rate * fade_out // 1000) if fade_out else 0
    step_in = Q28 // fi if fi else 0
    step_out = Q28 // fo if fo else 0
    a = dc_a(rate)
    x1, y1 = [0, 0], [0, 0]
    touched = gain != Q28 or dc or fi or fo or chans == 1
    dither = R.Dither(0x9E3779B9)
    out = []
    for i in range(n):
        f = end - 1 - i if reverse else begin + i
        w = [0, 0]
        for c in range(channels):
            x = frames16[f][c]
            v = x * 65536 if gain == Q28 else gain_word(x, gain)
            if dc:
                y = sat(sat(v - x1[c]) + mul31(y1[c], a))
                x1[c], y1[c] = v, y
                v = y
            g = Q28
            if i < fi:
                g = i * step_in
            back = n - 1 - i
            if back < fo and back * step_out < g:
                g = back * step_out
            if g < Q28:
                v = mul31(v, g << 3)
            w[c] = v
        o = [w[0], w[1]]
        if chans == 1:
            o[0] = (w[0] >> 1) + (w[1] >> 1) + (w[0] & w[1] & 1)
        elif chans == 3:
            o[0] = w[1]
        out.append(tuple(dither.sample(o[c]) if touched else o[c] >> 16 for c in range(out_channels)))
    return out


class Preview(unittest.TestCase):
    """Previews: each render swaps in under a fresh name and plays."""

    def test_preview_names_are_never_reused(self):
        text = STUDIO.read_text()
        self.assertIn('render.serial = ++preview_serial;', text)
        self.assertIn('scratch_path(render.final_path, sizeof render.final_path, studio.track, render.serial);', text)
        self.assertNotIn('kScratchNames', text)

    def test_no_render_while_the_last_preview_loads(self):
        text = STUDIO.read_text()
        body = re.search(r'void maybe_render\(u32 t\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('studio.loading_preview', body)
        self.assertIn('maybe_check(t);\n    maybe_render(t);', text)

    def test_pcm_borrowed_without_stocks_wanted_test(self):
        # stock's acquire refuses the key's own sample while the library is open (A14)
        text = STUDIO.read_text()
        self.assertNotIn('sdk_sample_cache_sample_acquire(', text)
        self.assertIn('const host::Borrowed h = host::borrow(key);', text)
        body = re.search(r'Borrowed borrow\(u32 key\) \{(.*?)\n\}', HOST.read_text(), re.S).group(1)
        self.assertNotIn('kNodeUnwanted] == 0', body)
        self.assertIn('node[kNodeLoaded]', body)
        self.assertIn('kNodeCount) += 1', body)
        self.assertNotIn('"still loading"', text)

    def test_every_file_change_tells_stock_to_gather(self):
        # without the flag a file reaches the key only at the next encoder turn (A15); saves, undo and kits set it
        text = STUDIO.read_text()
        self.assertIn('const u32 kSamplesChanged = 0x840a052d;', HOST.read_text())
        self.assertIn('host::samples_changed();', text)
        self.assertNotIn('host::send_regions(render.track', text)
        calls = re.findall(r'send_regions\(([^;]*)\);', text)
        self.assertEqual([c for c in calls if ',' not in c], [], 'every table send says whether a file changed')

    def test_the_key_is_untouched_while_editing_and_stale_renders_stop(self):
        # the key stays strictly untouched; the studio streams its previews (A16, A19)
        text = STUDIO.read_text()
        for gone in ('stage_preview', 'spare_slot', 'through_key', 'void assign_preview(', 'void write_markers('):
            self.assertNotIn(gone, text)
        flush = re.search(r'void flush_live\(bool\) \{(.*?)\}', text, re.S).group(1)
        self.assertNotIn('region_entry', flush)
        self.assertNotIn('studio.loading_preview = true', text)            # no stock load to wait for
        after = re.search(r'void after_render\(u32\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('render.superseded = true;', after)
        self.assertIn('if (render.quick) {', after)
        self.assertIn('play_file(studio.path', text)
        self.assertIn('} else if (key == kKeyM3) {', text)
        def scale(a, n, length):
            return scale_frames(a, n, length)
        for a, n, length in ((14487, 72437, 72437), (100000, 500000, 250000), (0, 882000, 441000),
                             (441000, 882000, 441000), (30, 60, 120)):
            want = a * n // length
            self.assertLessEqual(abs(scale(a, n, length) - want), max(16, want // 4000), (a, n, length))

    def test_first_waveform_gets_a_new_geometry_key(self):
        # sharing key 1 with the placeholders, the first waveform stays blank until the first encoder turn
        text = STUDIO.read_text()
        body = re.search(r'if \(!paths_ready\) \{(.*?)\n    \}', text, re.S).group(1)
        self.assertIn('++path_key;', body.split('paths_ready = true;')[1])
        self.assertIn('"waveform drawn: key %d', text)

    def test_stretch_reads_as_playback_speed(self):
        # the stretch readout is playback speed: x1.42 is faster than x1
        speed = lambda stretch: (10000 + stretch // 2) // stretch
        self.assertEqual([speed(x) for x in (25, 50, 70, 100, 200, 400)], [400, 200, 143, 100, 50, 25])
        text = STUDIO.read_text()
        self.assertIn('hundredths(values[0], 12, n, (10000u + e.stretch / 2u) / e.stretch);', text)
        self.assertIn('e.stretch = step_by(e.stretch, -detents,', text)   # clockwise: shorter, faster

    def test_saves_wait_for_a_preview_and_name_their_folder(self):
        # a save waits for its preview to finish, and its card names the folder it went to
        text = STUDIO.read_text()
        body = re.search(r'void start_save\(\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('studio.save_pending = true;', body)
        self.assertIn('studio.save_pending)) {', text)                  # the preview stops at its next job
        self.assertIn('(full render, %s, %d ms)', text)
        def where(path):
            folder = path.rsplit('/', 1)[0]
            shown = folder[7:] if folder.startswith('/fat32/') else folder
            if len(shown) > 14:
                shown = folder.rsplit('/', 1)[1]
            return 'saved in ' + shown[:14]
        self.assertEqual(where('/fat32/samples/user/unnamed-f2-2-ed1.wav'), 'saved in samples/user')
        self.assertEqual(where('/fat32/presets/dust/dust 18.preset/dust 18-ed1.wav'), 'saved in dust 18.preset')
        self.assertLessEqual(len(where('/fat32/a/very long folder name here/x-ed1.wav')), 23)

    def test_playhead_follows_the_player(self):
        text = STUDIO.read_text()
        body = re.search(r'void playhead\(u32 t\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('if (!play.on || !studio.ready) return;', body)
        self.assertIn('show(panel::N_PLAYHEAD', body)
        for rate in (44100, 48000, 96000):
            for ms in (0, 999, 60000):
                self.assertLess(ms * (rate // 10), 1 << 32)

    def test_output_pages_resize_then_fit_with_one_waveform(self):
        # one waveform, never two overlapping: it resizes in proportion as the length changes,
        # then eases to fit once the render is complete (A20)
        text = STUDIO.read_text()
        stage = re.search(r'void output_stage\(\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertNotIn('N_DIM0', stage)
        self.assertIn('wave(panel::N_LIT0 + qd, -lit / 8, lit / 8, amp, 32, path_key);', stage)
        self.assertIn('red_quarter(', stage)
        self.assertIn('green_quarter(', stage)
        self.assertIn('landed(render.length);', text)
        view = re.search(r'View output_view\(\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('axis_now(now_ms())', view)

    def test_ui_defaults_and_header(self):
        text = STUDIO.read_text()
        self.assertIn('e.snap = 1;', text)
        self.assertIn('e.chop_mode = 1;', text)
        self.assertIn('draw::page_header(header_state, in, output, titles, shown_values, panel::N_BAR0);', text)
        self.assertNotIn('void header_row(', text)
        self.assertIn('show(panel::N_ZERO0', text)
        hpp = PANEL_HPP.read_text()
        # the SDK header's layout: titles 0..3, values 4..7, the under row 8..15; bars consecutive
        for name, slot in (('T_TITLE0', 0), ('T_VALUE0', 4), ('T_UNDER_TITLE0', 8), ('T_UNDER_VALUE0', 12)):
            self.assertIn(f'const u32 {name} = {slot};', hpp)
        bars = [int(re.search(rf'N_BAR{i} = (\d+);', hpp).group(1)) for i in range(8)]
        self.assertEqual(bars, list(range(bars[0], bars[0] + 8)))

    def test_saves_write_their_whole_length(self):
        # a x0.54 save of a 701,049-frame source must not stop at 882,000 frames (the preview limit)
        text = STUDIO.read_text()
        self.assertIn('u32 frame_limit() { return render.target == kToPreview ? kMaxFrames : kSaveMaxFrames; }', text)
        self.assertIn('render.length >= frame_limit()) {', text)   # the count pass stops at the target's limit
        self.assertIn('key_frames(render.length)', text)
        self.assertIn('key_frames(render.slice_frames[k])', text)
        limit = int(re.search(r'kSaveMaxFrames = (\d+);', text).group(1))
        self.assertLess(limit * 1024, 1 << 32)            # the waveform bins: frame * kBins
        self.assertGreaterEqual(limit, 882000 * 4)         # a 20 s source at x0.25
        # slice cuts on long outputs: n * q16 / 2^16, split
        for n in (1000, 882000, 1296940, limit):
            for q16 in (0, 1, 32768, 65535, 65536):
                at = (n >> 16) * q16 + (((n & 0xFFFF) * q16) >> 16)
                self.assertLess((n & 0xFFFF) * q16, 1 << 32)
                self.assertEqual(at, n * q16 >> 16, (n, q16))

    def test_frame_scaling_is_exact_to_the_save_limit(self):
        # mapping f * (src / 64 + 1) would pass 2^32 once sources pass ~6 s
        text = STUDIO.read_text()
        self.assertIn('const u32 s0 = scale_frames(f0, src_len, n), s1 = scale_frames(f1, src_len, n);', text)
        for a, n, length in ((648470, 701049, 1296940), (1296940, 701049, 1296940), (3999999, 882000, 4000000),
                             (12345, 4000000, 882000), (881999, 3528000, 882000), (0, 701049, 1296940)):
            want = a * n // length
            self.assertLessEqual(abs(scale_frames(a, n, length) - want), max(64, want // 256), (a, n, length))

    def test_notes_play_the_edit_and_the_track_is_held_silent(self):
        # the keys play the edit, not the filtered original through the track (A22, P7)
        studio = STUDIO.read_text()
        player = PLAYER.read_text()
        mixer = (PLATFORM / 'mixer.cpp').read_text()
        follow = re.search(r'void follow_notes\(u32 t\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertIn('send_hold(studio.track + 1)', follow)
        self.assertIn('if (studio.mode == kOpen && studio.ready) {', follow)
        self.assertEqual(follow.count('play_edit();'), 2)   # A33: a pad's slice, or the edit
        self.assertIn('u32 track_notes() { return host::note_serial(studio.track); }', studio)
        self.assertIn('stock<u32(u32)>(0x8d4d6c08)(track)', HOST.read_text())
        frame = studio[studio.index('    flush_live(false);\n    if (grow_due) {'):]
        self.assertLess(frame.index('follow_notes(t);'), frame.index('keep_playing(t);'))
        close = re.search(r'void close_studio\(bool keep\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertIn('send_hold(0);', close)
        opened = re.search(r'void begin_studio\(u32 track, bool file\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertIn('note_seen = keyless || track >= 8 ? 0 : track_notes();', opened)   # a sounding note doesn't play it
        # the audio board: the mixer's voice site hands the player each rendered voice, which zeroes
        # only the held track and lets go without a resend
        self.assertIn('    sdk_studio_voice(kVoiceOutput, track);\n', mixer)
        voice = re.search(r'void voice\(i32\* out, u32 track\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('mute_now = hold_age <= kTimeoutBlocks ? hold_value : 0;', voice)
        self.assertIn('if (!was && !now) return;', voice)
        self.assertIn('if (hold_age <= kTimeoutBlocks) hold_age = hold_age + 1;', player)
        self.assertLess(((1 << 31) >> 6) * 63, 1 << 31)   # the ramp stays in 31 bits
        profile = json.loads((ROOT / 'profiles/opxy-1.2.1.json').read_text())
        self.assertIn('sampleStudio', profile['services']['mixer'])

    def test_play_while_rendering_level_and_one_bar(self):
        # play what's rendered so far, as segment files; one progress bar; the key's level (A23, P4, P8)
        studio = STUDIO.read_text()
        player = PLAYER.read_text()
        segment = re.search(r'bool next_segment\(\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertIn('file_close(render.file);', segment)
        self.assertIn('segment_path(path, sizeof path, render.final_path, render.segments);', segment)
        self.assertNotIn('r+b', segment)                                       # no reopen-append: its seek doesn't hold
        self.assertNotIn('send_grow', segment)                                 # B3 only from the GUI task
        self.assertNotIn('0x8fed06cc', studio)
        self.assertIn('return flush_chunk() && next_segment();', studio)
        self.assertEqual(65536 % 8192, 0)                                       # segments end on chunk writes
        self.assertLessEqual(-(-882000 // 65536), 16)                          # letters a..p at most
        self.assertIn('if (render.streaming && render.segmented) {', studio)   # no header patch of a missing file
        play = re.search(r'void play_edit\(\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertLess(play.index('growing_now()'), play.index('showing_preview'))
        self.assertIn('if (segmented) send_grow(play.which, segmented);', studio)
        self.assertIn('if (studio.shown_segmented) send_grow(studio.shown_serial, studio.preview_frames);', studio)
        # the loader reads whole segments at their place, up to the count on disk
        self.assertIn('const u32 segment = segmented ? loaded_frames / kSegmentFrames : ~0u;', player)
        self.assertNotIn('seek_file', player)
        self.assertIn('const u32 kSegmentFrames = 65536;', player)
        # the level: the key's at the default track gain, fixed
        self.assertIn('const i32 kKeyGain = 0x08cccccc >> 9;', player)
        self.assertAlmostEqual((0x08cccccc >> 9) * 65536 / 2 ** 31, 128 * 0x08cccccc / 2 ** 31, delta=0.001)
        self.assertIn('word_out = opxy::q31::multiply(word_out, kKeyGain);', player)
        block = re.search(r'void master\(i32\* left, i32\* right\) \{(.*?)\n\}', player, re.S).group(1)
        for forbidden in ('log_line', 'open_file', 'post_load'):
            self.assertNotIn(forbidden, block)
        # the bar after the first part goes on from where it was, up to the end
        for n, first in ((1296940, 66150), (701049, 66150), (100000, 120000)):
            base = 65536 if first >= n else scale_frames(first, 65536, n)
            values = [base + scale_frames(q, 65536 - base, 65536) for q in range(0, 65537, 4096)]
            self.assertEqual(values, sorted(values))
            self.assertGreaterEqual(values[0], base)
            self.assertLessEqual(values[-1], 65536)

    def test_a_note_off_stops_like_a_gate(self):
        # lifting a finger off the pad stops the sample
        studio = STUDIO.read_text()
        player = PLAYER.read_text()
        follow = re.search(r'void follow_notes\(u32 t\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertIn('play.by_note = play.on;', follow)
        self.assertIn('else if (play.held_seen || t - play.t0 > kTapMs) stop_play();', follow)
        self.assertIn('host::note_held(studio.track, n)', studio)
        self.assertIn('const u32 kNoteMap = 0x86bda258;', HOST.read_text())
        self.assertIn('play.by_note = play.held_seen = false;', studio)        # Shift + M3 plays to the end
        self.assertIn('const u32 kStopStep = 1;', player)

    def test_player_never_passes_its_ids_to_stock_and_stays_in_32_bits(self):
        player = PLAYER.read_text()
        # 138..144 are consumed; stock's setter 8d50eb8c has no bounds check
        body = re.search(r'void receive\(u32 channel, i32 value\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('const u32 id = 138 + channel;', body)
        self.assertIn('if (id < kBegin || id > kCommit) return;', body)
        self.assertNotIn('0x8d50eb8c', player)
        # the SDK's receiver gives 138..152 to the player and only the rest to stock
        receiver = re.search(r'void sdk_master_receive\(u32 base, u32 id, i32 value\) \{(.*?)\n\}', AUDIO.read_text(), re.S).group(1)
        self.assertEqual(receiver.count('0x8d50eb8c'), 1)
        self.assertLess(receiver.index('id >= kFirstId && id < kFirstId + kChannels'), receiver.index('0x8d50eb8c'))
        self.assertIn('const u32 kFirstId = 138, kChannels = 15;', AUDIO.read_text())
        self.assertIn('const u32 kBegin = 138, kSource = 139, kWhich = 140, kStart = 141, kEnd = 142, kLevel = 143, kCommit = 144;',
                      player)
        studio = STUDIO.read_text()
        sent = sorted(set(int(x) for x in re.findall(r'b3\((\d+),', studio)))
        self.assertEqual(sent, list(range(138, 153)))   # 150..152: the real-time bounce (A29, P11)   # 145: the hold (A22, P7); 146, 147: the growing preview (A23, P8); 148, 149: the live chain (A24, P9)
        self.assertLess(body.index('if (id == kHold) {'), body.index('if (id < kBegin || id > kCommit) return;'))
        # the loader on the audio board: stock's background worker, the sample pool, never the block
        self.assertIn('host::post(load_step, nullptr);', player)
        block = re.search(r'void master\(i32\* left, i32\* right\) \{(.*?)\n\}', player, re.S).group(1)
        for forbidden in ('open_file', 'read_file', 'pool_alloc', 'pool_free', 'post_load', 'log_line'):
            self.assertNotIn(forbidden, block)
        self.assertIn('if (used + kBufferBytes + kHeadroom > kPoolCapacity) {', player)   # a headroom check first
        self.assertIn('kHeader - base) & 31u', player)                                    # 44 mod 32: direct reads
        self.assertLess(882000 * 4 + 64, 1 << 32)
        # the stream is gone: the receiver hook and its fixture are back as they were
        hooks = (PLATFORM / 'sample_hooks.s').read_text()
        self.assertNotIn('.Lslice_stock', hooks)
        self.assertNotIn('sdk_studio_stream_receive', (PLATFORM / 'drum_controls_stock.cpp').read_text())
        # the mix: interpolation and ramp products in 32 bits
        self.assertLess(65535 * 32767, 1 << 31)
        self.assertLess(((1 << 31) >> 8) * 255, 1 << 31)
        for rate in (8000, 44100, 48000, 96000, 192000):
            self.assertLess(rate << 10, 1 << 32)
        profile = json.loads((ROOT / 'profiles/opxy-1.2.1.json').read_text())
        bus = [p for p in profile['patches'] if p.get('service') == 'master_bus']
        self.assertEqual(sorted(p['address'] for p in bus), ['0x8d511b80', '0x8d5b145e'])
        selection = (ROOT / 'tooling/extensions.py').read_text()
        self.assertIn("require(not (studios and processors),", selection)   # the two share these sites

    def test_print_through_fx(self):
        # A29 (A21 retired): every effect prints by the real-time bounce; no effect's code in the SDK
        source = (ROOT / 'tooling/adapters.py').read_text()
        self.assertNotIn('offline_entry', source)
        self.assertNotIn('OFFLINE_ENTRY', (PLATFORM / 'fx_object.cpp.in').read_text())
        studio = STUDIO.read_text()
        for name in ('lint_fx_offline', 'lint_fx_id', 'fx_block', 'render.fx_on', 'tail_frames'):
            self.assertNotIn(name, studio)
        self.assertIn('bool wants_bounce(const Edit& e) { return e.send1 || e.send2; }', studio)
        self.assertIn('const char* y[4] = {"fx i", "fx ii", "tail", "comp"};', studio)   # A25's layout
        self.assertIn('for (u32 k = 0; k < 2 && target == kToSave; ++k) {', studio)      # a rendered save is dry
        self.assertIn('append(second, sizeof second, m, effect_name(dry_engine));', studio)
        # A29: a stock effect on a send bounces in real time; the audio board captures and writes
        self.assertIn('if (can_print(studio.edit) && start_bounce()) return;',
                      studio)
        player = PLAYER.read_text()
        cap = re.search(r'void capture_block\(\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('const i32* const fx0 = host::fx_bus(0);', cap)
        self.assertIn('saturate_add(saturate_add(studio_l[i], fx0[2 * i]), fx1[2 * i])', cap)
        self.assertNotIn('write_file', cap)                                   # files only on bg_worker
        self.assertEqual(288358 * 7447 < 1 << 31, True)                       # to16 stays in 32 bits
        self.assertLess(abs(0x46666 * 7447 / 2 ** 31 * 65536 - 65536), 64)   # x 7447 undoes the key's level

    def test_the_alert_slot_is_authored_red_and_each_alert_fits(self):
        tree = ET.fromstring(PANEL_SVG.read_text())
        by_id = {e.get('id'): e for e in tree.iter() if e.get('id')}
        self.assertEqual(by_id['ss_file_alert'].get('fill'), '#FF3020')
        self.assertIn('T_FILE_ALERT', PANEL_HPP.read_text())
        text = STUDIO.read_text()
        alerts = ['rendering', 'about to render', 'loading the render', 'render failed', 'render not loaded',
                  'rendering the rest']
        for a in alerts:
            self.assertIn(f'"{a}"', text)
            self.assertLessEqual(len(a), 23)
        self.assertIn('say(panel::T_FILE_ALERT', text)


class Arithmetic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REFERENCE))
        import studio_ref
        cls.R = studio_ref

    def test_tables_match_the_reference(self):
        gains, targets, silence = header_tables()
        self.assertEqual(gains, [self.R.db_to_q28(d) for d in range(-30, 21)])
        self.assertEqual(targets, [int(math.floor(32767 * 10 ** (t / 20))) for t in (0, -1, -3, -6)])
        self.assertEqual(silence, [int(math.floor(32767 * 10 ** (d / 20))) for d in range(-60, -23, 6)])

    def test_dc_coefficient(self):
        for rate in (8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000):
            self.assertEqual(dc_a(rate), self.R.dc_coefficient(rate))

    def test_gain_word_is_mul_q28(self):
        rnd = random.Random(5)
        gains = header_tables()[0]
        for _ in range(4000):
            x = rnd.randint(-32768, 32767)
            g = rnd.choice([rnd.randint(0, 0xffffffff), rnd.choice(gains)])
            self.assertEqual(gain_word(x, g), self.R.mul_q28(x << 16, g), (x, g))

    def test_chain_nulls_against_the_reference(self):
        R = self.R
        rnd = random.Random(11)
        rate = 44100
        for case in range(24):
            channels = 1 + case % 2
            frames = 900
            loud = case % 3 == 0
            frames16 = [tuple((rnd.choice([32767, -32768, rnd.randint(-32768, 32767)]) if loud else
                               int(9000 * math.sin(i / 7 + c)) + 1500) for c in range(channels)) for i in range(frames)]
            begin, end = rnd.randint(0, 200), rnd.randint(500, frames)
            reverse = rnd.random() < 0.5
            level = rnd.randint(-30, 20)
            normal = rnd.choice([0, 0, 1, 2, 3, 4])
            dc = rnd.random() < 0.5
            fade_in, fade_out = rnd.choice([0, 1, 3, 5]), rnd.choice([0, 2, 4])
            chans = rnd.choice([0, 1, 2, 3]) if channels == 2 else 0
            got = device_bake(R, frames16, rate, channels, begin, end, reverse, level, normal, dc, fade_in, fade_out,
                              chans)
            s = R.Sample([tuple(v << 16 for v in row) for row in frames16], rate, channels)
            R.bake(s, begin, end, rev=reverse, dc=dc, gain_db=None if normal else level,
                   normalise_db=[0, -1, -3, -6][normal - 1] if normal else None, fade_in=fade_in, fade_out=fade_out,
                   chans=['keep', 'mono', 'L', 'R'][chans])
            dither = R.Dither(0x9E3779B9)
            want = [tuple(dither.sample(w) if s.touched else w >> 16 for w in row) for row in s.frames]
            self.assertEqual(got, want, f'case {case}')


DSP = EXT / 'studio_dsp.hpp'


class Stage3(unittest.TestCase):
    """The render chain's tables and the reference that remakes a save."""
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REFERENCE))
        import studio_chain
        cls.C = studio_chain

    def table(self, name):
        body = re.search(name + r' = \{([^}]*)\}', DSP.read_text(), re.S).group(1)
        return [int(v.strip().rstrip('u')) for v in body.replace('\n', ' ').split(',') if v.strip()]

    def test_tables_match_the_reference(self):
        self.assertEqual(self.table(r'kSemitoneQ16\[49\]'), self.C.SEMITONE_Q16)
        self.assertEqual(self.table(r'kCutoffHz\[48\]'), self.C.CUTOFF_HZ)
        self.assertEqual(self.table(r'kCutoffQ31\[48\]'), self.C.CUTOFF_Q31)
        self.assertLess(max(self.C.CUTOFF_Q31), 1 << 31)
        self.assertEqual(self.table(r'kRateHz\[9\]'), self.C.RATE_HZ)
        self.assertEqual(self.table(r'kBits\[9\]'), self.C.BITS)

    def sample(self, n=6000, channels=2):
        R = sys.modules['studio_ref']
        frames = [tuple(int(12000 * math.sin(i * 0.031 + c) + 3000 * math.sin(i * 0.37)) << 16 for c in range(channels))
                  for i in range(n)]
        return R.Sample(frames, 44100, channels)

    def test_nothing_set_is_a_bit_exact_copy(self):
        smp = self.sample()
        out, _ = self.C.render(smp, self.C.Settings(begin=0, end=6000))
        self.assertEqual(out, [tuple(v >> 16 for v in row) for row in smp.frames])

    def test_time_lengths(self):
        smp = self.sample(20000)
        for kw, want in ((dict(stretch=200, beat=0), 40000), (dict(stretch=50, beat=0), 10000),
                         (dict(stretch=150), 30000), (dict(pitch=-12), 20000)):
            out, _ = self.C.render(smp, self.C.Settings(begin=0, end=20000, **kw))
            self.assertLess(abs(len(out) - want), 2700, kw)   # beat loses at most a grain at the end

    def test_bits_quantise_and_drive_saturates(self):
        smp = self.sample()
        out, _ = self.C.render(smp, self.C.Settings(begin=0, end=6000, bits_index=8), level=0)   # 2 bits
        self.assertLessEqual(len({v >> 14 for row in out for v in row}), 6)
        out, _ = self.C.render(smp, self.C.Settings(begin=0, end=6000, drive=100))
        self.assertTrue(all(-32768 <= v <= 32767 for row in out for v in row))

    def test_full_scale_at_max_drive_is_loud_and_never_wraps(self):
        # full drive is loud and dirty, never so clipped that it ends up mostly silent
        class Sine:
            channels, rate = 1, 44100
        rms = lambda v: math.sqrt(sum(x * x for x in v) / len(v))
        for amp in (1.0, 0.999, 0.25):
            smp = Sine()
            src = [int(amp * 32767 * math.sin(2 * math.pi * 220 * i / 44100)) for i in range(4410)]
            smp.frames = [(v << 16,) for v in src]
            clean = [r[0] for r in self.C.render(smp, self.C.Settings(begin=0, end=4410))[0]]
            for drive in (1, 50, 100):
                out = [r[0] for r in self.C.render(smp, self.C.Settings(begin=0, end=4410, drive=drive))[0]]
                self.assertGreater(rms(out), 0.6 * rms(clean), (amp, drive))         # never quieter, never silent
                self.assertLessEqual(max(abs(v) for v in out), 32767, (amp, drive))   # 100 leaves ~3 dB
                for a, b in zip(src, out):                                           # no wrap: the sign follows
                    if abs(a) > 3000:
                        self.assertEqual(a > 0, b > 0, (amp, drive, a, b))
            if amp < 0.5:
                self.assertGreater(rms(out), 1.8 * rms(clean))                        # quiet input: +5 dB or more

    def test_drive_curve_is_smooth_and_matches_the_cpp(self):
        words = [i << 20 for i in range(-2048, 2048)]
        for drive in (1, 37, 100):
            curve = [self.C.drive_word(w, drive) for w in words]
            self.assertEqual(curve, sorted(curve), drive)                              # monotonic
            steps = [b - a for a, b in zip(curve, curve[1:])]
            self.assertLess(max(steps), 4 * (1 << 20) * (1 + 5 * drive / 100), drive)  # no jump at the knee
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        for name, value in (('kDriveGainStep', self.C.DRIVE_GAIN_STEP), ('kDriveMakeupStep', self.C.DRIVE_MAKEUP_STEP),
                            ('kDriveBits', self.C.DRIVE_BITS)):
            self.assertRegex(dsp, rf'{name} = {value}u', name)
        self.assertIn('for (u32 c = 0; c < ch; ++c) w[c] = drive_word(w[c], s.drive);', dsp)

    def _tone(self, rate, channels, frames, freqs=(440, 660)):
        class Tone:
            pass
        t = Tone()
        t.rate, t.channels = rate, channels
        t.frames = [tuple(int(0.5 * 32767 * math.sin(2 * math.pi * freqs[c] * i / rate)) << 16 for c in range(channels))
                    for i in range(frames)]
        return t

    @staticmethod
    def _pitch(v, rate):
        z = [i for i in range(1, len(v)) if v[i - 1] < 0 <= v[i]]
        return (len(z) - 1) * rate / (z[-1] - z[0])

    def test_time_stretch_on_stereo_and_mono_at_both_rates(self):
        # x0.5, x2 and +-7 semitones, tape and beat,
        # stereo and mono, 44.1 and 48 kHz: length, pitch on both channels, no splice jumps
        for rate in (44100, 48000):
            for channels in (1, 2):
                n = rate // 3
                smp = self._tone(rate, channels, n)
                for kw in (dict(stretch=50), dict(stretch=200), dict(stretch=50, beat=0), dict(stretch=200, beat=0),
                           dict(pitch=7), dict(pitch=-7)):
                    out, oc = self.C.render(smp, self.C.Settings(begin=0, end=n, **kw))
                    self.assertEqual(oc, channels)
                    want = n * kw.get('stretch', 100) // 100
                    self.assertLess(abs(len(out) - want), rate * 0.13, (rate, channels, kw))   # a grain at most
                    ratio = 2 ** (kw.get('pitch', 0) / 12) if kw.get('beat', 1) else 100 / kw['stretch']
                    for c in range(channels):
                        v = [r[c] for r in out]
                        f = self._pitch(v, rate)
                        self.assertLess(abs(f / ((440, 660)[c] * ratio) - 1), 0.012, (rate, channels, kw, c, f))
                        lim = 2 * math.pi * (440, 660)[c] * ratio / rate * 16384 * 1.6 + 200
                        self.assertFalse(any(abs(v[i] - v[i - 1]) > lim for i in range(1, len(v))), (rate, channels, kw, c))

    def test_beat_grain_grows_with_compression(self):
        # at stretch 25% (175,518 of 701,049 frames), 40 ms grains would give 20 ms slices of every 80 ms
        S = self.C.Settings
        for kw, want in ((dict(), 40), (dict(stretch=25), 160), (dict(stretch=50), 80), (dict(stretch=80), 50),
                         (dict(pitch=-12), 80), (dict(stretch=200, pitch=-12), 40), (dict(pitch=12), 40),
                         (dict(stretch=25, grain_ms=120), 160), (dict(stretch=200), 40), (dict(stretch=25, pitch=-24), 160)):
            self.assertEqual(self.C.grain_ms_for(S(**kw)), want, kw)
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('const u32 kMaxGrainMs = 160;', dsp)
        self.assertIn('grain = rate * grain_ms_for(s) / 1000;', dsp)
        # x0.25 on stereo tones at both rates: a quarter as long, the pitch kept, no splice jumps, 80 ms hops
        for rate in (44100, 48000):
            n = rate
            smp = self._tone(rate, 2, n)
            out, _ = self.C.render(smp, S(begin=0, end=n, stretch=25))
            self.assertLess(abs(len(out) - n // 4), rate * 0.17, rate)
            p = self.C.Pipeline([tuple(w >> 16 for w in r) for r in smp.frames], 2, rate, S(begin=0, end=n, stretch=25))
            self.assertEqual(p.hop, (rate * 160 // 1000 & ~1) // 2)
            for c in range(2):
                v = [r[c] for r in out]
                f = self._pitch(v, rate)
                self.assertLess(abs(f / (440, 660)[c] - 1), 0.012, (rate, c, f))
                lim = 2 * math.pi * (440, 660)[c] / rate * 16384 * 1.6 + 200
                self.assertFalse(any(abs(v[i] - v[i - 1]) > lim for i in range(1, len(v))), (rate, c))

    def test_beat_advance_is_exact_at_the_extremes(self):
        # 160 ms grains at 48 kHz with x0.25 and -24 st: hop * per_out passes 2^32 whole
        S = self.C.Settings
        for rate in (44100, 48000):
            for kw in (dict(stretch=25, pitch=-24), dict(stretch=25), dict(stretch=400, pitch=24), dict(pitch=-24)):
                st = S(begin=0, end=1000, **kw)
                p = self.C.Pipeline([(0, 0)] * 1000, 2, rate, st)
                ratio = self.C.SEMITONE_Q16[kw.get('pitch', 0) + 24]
                per_out = (6553600 << 8) // ((st.stretch * ratio) >> 8)
                self.assertEqual(p.advance_int * 65536 + p.advance_frac, p.hop * per_out, (rate, kw))
                self.assertLess(p.advance_int, 1 << 31)
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('advance_int = hop * (per_out_q16 >> 16) + (low >> 16);', dsp)

    def test_one_pass_and_quick_part_match_the_counted_render(self):
        # A18's one pass writes exactly the frames the count measured; A17's first part is the start of the whole
        smp = self._tone(44100, 2, 20000)
        for kw in (dict(stretch=200), dict(stretch=50, beat=0), dict(pitch=-7)):
            st = self.C.Settings(begin=0, end=20000, **kw)
            st.gain = self.C.Q28
            counted, p = 0, self.C.Pipeline([tuple(w >> 16 for w in r) for r in smp.frames], 2, 44100, st)
            while p.next() is not None:
                counted += 1
            p = self.C.Pipeline([tuple(w >> 16 for w in r) for r in smp.frames], 2, 44100, st)
            streamed = []
            while len(streamed) < self.C.MAX_FRAMES:
                w = p.next()
                if w is None:
                    break
                streamed.append(w)
            self.assertEqual(len(streamed), counted, kw)
            out, _ = self.C.render(smp, self.C.Settings(begin=0, end=20000, **kw))
            self.assertEqual(len(out), counted, kw)
            quick = min(44100 * 1500 // 1000, counted)
            p = self.C.Pipeline([tuple(w >> 16 for w in r) for r in smp.frames], 2, 44100, st)
            self.assertEqual([p.next() for _ in range(quick)], streamed[:quick], kw)   # a fresh chain, cut short
        text = STUDIO.read_text()
        self.assertIn('render.counting && (render.split || (!e.fade_out && render.target != kToSlices && !header_patch_failed))', text)
        self.assertIn('file_open(render.target == kToSave ? render.temporary : render.final_path, "r+b")', text)
        self.assertIn('render.length = render.done;', text)

    def test_slices_cover_the_output(self):
        smp = self.sample()
        out, _ = self.C.render(smp, self.C.Settings(begin=0, end=6000), slices=[0, 1500, 4000], chop_fade=2)
        self.assertEqual([len(s) for s in out], [1500, 2500, 2000])
        self.assertTrue(all(abs(v) <= 1 for v in out[1][0]))   # each slice fades in from silence (dither aside)


if __name__ == '__main__':
    unittest.main()


def tape_frames(length, stretch):
    """editor.cpp tape_frames, in its 32-bit parts."""
    if stretch in (0, 100):
        return length
    speed = 6553600 // stretch
    if length < 2:
        return 0
    l = length - 1
    q1, r1 = divmod(l, speed)
    q2, r2 = divmod(r1 << 8, speed)
    q3, r3 = divmod(r2 << 8, speed)
    assert (r1 << 8) < 1 << 32 and (r2 << 8) < 1 << 32
    return (q1 << 16) + (q2 << 8) + q3 + (1 if r3 else 0)


class Live(unittest.TestCase):
    """A24/P9: the player's live chain against the saved render, block by block."""
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REFERENCE))
        import studio_chain
        cls.C = studio_chain
        cls.R = sys.modules['studio_ref']

    def tone(self, n, channels, seed):
        rnd = random.Random(seed)
        return [tuple(max(-32768, min(32767, int(14000 * math.sin(i * (0.013 + 0.007 * c)) + 6000 * math.sin(i * 0.41) +
                                                  rnd.randint(-900, 900)))) for c in range(channels)) for i in range(n)]

    def live(self, data, channels, rate, kw, level=0, normal=0, fade_in=0, fade_out=0, chans='keep', region=None,
             chop_fade=0):
        """What the player plays: player.cpp live_block over 64-frame blocks, the settings
        editor.cpp live_values sends (as words, before the key's level)."""
        C = self.C
        st = C.Settings(**kw)
        st.beat = 0                                     # the receiver's commit forces tape
        st.pitch = 0
        if region:
            st.begin, st.end = region
        if normal:
            peak = max((abs(v) for row in data[kw['begin']:kw['end']] for v in row), default=0)
            st.gain = C.normalise_gain(peak, C.NORMAL_TARGET[normal - 1])
        else:
            st.gain = C.LEVEL_GAIN[level + 30]
        n = tape_frames(st.end - st.begin, st.stretch)
        fi = rate * (chop_fade if region else fade_in) // 1000
        fo = rate * (chop_fade if region else fade_out) // 1000
        fi, fo = min(fi, n), min(fo, n)
        step_in = C.Q28 // fi if fi else 0
        step_out = C.Q28 // fo if fo else 0
        mode = {'keep': 0, 'mono': 1, 'L': 2, 'R': 3}[chans] if channels > 1 else 0
        p = C.Pipeline(data, channels, rate, st)
        out, k = [], 0
        while k < n:                                    # one block at a time, the chain's state carried
            for _ in range(64):
                if k >= n:
                    break
                w = p.next()
                if w is None:
                    return out, n
                w = list(w)
                g = C.Q28
                if k < fi:
                    g = k * step_in
                back = n - 1 - k
                if back < fo and back * step_out < g:
                    g = back * step_out
                if g < C.Q28:
                    for c in range(channels):
                        w[c] = C.mul31(w[c], g << 3)
                if channels < 2:
                    w[1] = w[0]
                elif mode == 1:
                    w[0] = w[1] = (w[0] >> 1) + (w[1] >> 1) + (w[0] & w[1] & 1)
                elif mode == 2:
                    w[1] = w[0]
                elif mode == 3:
                    w[0] = w[1]
                out.append(tuple(w))
                k += 1
        return out, n

    def saved(self, data, channels, rate, kw, dither=True, **extra):
        C = self.C
        smp = self.R.Sample([tuple(v << 16 for v in row) for row in data], rate, channels)
        st = C.Settings(**kw)
        st.beat = 0
        original = C.R.Dither
        if not dither:
            class Plain:
                def __init__(self, seed):
                    pass

                def sample(self, w):
                    return w >> 16
            C.R.Dither = Plain
        try:
            return C.render(smp, st, **extra)
        finally:
            C.R.Dither = original

    def check(self, kw, channels=2, n=5000, seed=3, **extra):
        data = self.tone(n, channels, seed)
        live, frames = self.live(data, channels, 44100, kw, **extra)
        exact, out_channels = self.saved(data, channels, 44100, kw, dither=False, **extra)
        dithered, _ = self.saved(data, channels, 44100, kw, dither=True, **extra)
        self.assertEqual(len(live), len(exact), kw)
        self.assertEqual(frames, len(exact), kw)
        words = [tuple(w[c] >> 16 for c in range(out_channels)) for w in live]
        # sample for sample against the render before its 16-bit write
        self.assertEqual(words, exact, (kw, extra))
        # and within 1 LSB of the saved, dithered file around the rounded word: the file's write
        # (studio_ref Dither: round, plus TPDF noise of under one LSB either way) is all that differs;
        # the player plays the whole 32-bit word
        rounded = [tuple(min(32767, (w[c] + 0x8000) >> 16) for c in range(out_channels)) for w in live]
        worst = max((abs(a - b) for x, y in zip(rounded, dithered) for a, b in zip(x, y)), default=0)
        self.assertLessEqual(worst, 1, (kw, extra))

    def test_output_length_is_the_resamplers_count(self):
        C = self.C
        for length in (2, 3, 17, 1000, 4097, 65537, 300001):
            for stretch in (25, 26, 33, 50, 67, 99, 101, 150, 199, 250, 333, 400):
                st = C.Settings(begin=0, end=length, stretch=stretch, beat=0)
                if length <= 4097:
                    p = C.Pipeline([(0, 0)] * length, 2, 44100, st)
                    count = 0
                    while p.next() is not None:
                        count += 1
                    self.assertEqual(tape_frames(length, stretch), count, (length, stretch))
                speed = 6553600 // stretch
                self.assertEqual(tape_frames(length, stretch), -(-(length - 1) * 65536 // speed), (length, stretch))
        self.assertIn('return (q1 << 16) + (q2 << 8) + q3 + (r3 ? 1u : 0u);', STUDIO.read_text())

    def test_each_live_setting_matches_the_render(self):
        base = dict(begin=300, end=4700)
        cases = [
            dict(), dict(reverse=1), dict(dc=1), dict(begin=0, end=5000),
            dict(stretch=50), dict(stretch=73), dict(stretch=200), dict(stretch=400), dict(stretch=25),
            dict(rate_index=3), dict(rate_index=8), dict(bits_index=4), dict(bits_index=5), dict(bits_index=8),
            dict(drive=35), dict(drive=100),
            dict(sweep=-12), dict(sweep=-48), dict(sweep=30), dict(sweep=48), dict(sweep=1),
            dict(comp=20), dict(comp=100), dict(comp=60, drive=80, sweep=-20),
            dict(rate_index=5, bits_index=3, drive=50, sweep=25, comp=70),
            dict(stretch=150, reverse=1, dc=1, drive=60, sweep=-18, comp=40),
        ]
        for case in cases:
            kw = dict(base, **case)
            self.check(kw)
        for extra in (dict(level=-12), dict(level=7), dict(normal=2), dict(fade_in=20, fade_out=35),
                      dict(chans='mono'), dict(chans='L'), dict(chans='R')):
            self.check(dict(base, drive=20, stretch=120), **extra)
        self.check(dict(base, sweep=22, stretch=80, comp=50), channels=1)
        self.check(dict(base, drive=100, bits_index=2), channels=1, normal=1)

    def test_a_slice_plays_its_region(self):
        # the live slice starts the chain at the slice; the saved slices are cut from one pass. With
        # no stage that carries state across the cut (filter, wow, rate hold, DC) they're the same.
        C = self.C
        data = self.tone(6000, 2, 9)
        kw = dict(begin=0, end=6000, bits_index=3, drive=40)
        cuts = [0, 1500, 4000]
        saved, _ = self.saved(data, 2, 44100, kw, dither=False, slices=cuts, chop_fade=2)
        for k, (a, b) in enumerate(zip(cuts, cuts[1:] + [6000])):
            live, _ = self.live(data, 2, 44100, kw, region=(a, b), chop_fade=2)
            self.assertEqual([tuple(v >> 16 for v in w) for w in live], saved[k], k)

    def test_the_cpp_plays_the_render_chain(self):
        player = PLAYER.read_text()
        studio = STUDIO.read_text()
        live = re.search(r'void live_block\(i32\* left, i32\* right\) \{(.*?)\n\}', player, re.S).group(1)
        pull = re.search(r'bool pull\(i32\* w\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('chain.fill(chain_l, chain_r,', pull)                      # dsp::Pipeline, the render's chain
        self.assertIn('if (chain_out < l.fade_in) g = chain_out * chain_step_in;', pull)
        self.assertIn('if (back < l.fade_out && back * chain_step_out < g) g = back * chain_step_out;', pull)
        self.assertIn('w[c] = fade_word(w[c], g);', pull)
        self.assertIn('w[0] = w[1] = mono_word(w[0], w[1]);', pull)
        self.assertIn('if (!pull(w)) player.stopping = true;', live)
        self.assertIn('x = opxy::q31::multiply(x, kKeyGain);', live)
        self.assertIn('chain_now = chain_target = l;', player)                   # no easing at the start
        self.assertIn('chain.start(view.data, view.channels, view.rate, l.s);', player)
        self.assertIn('staged_live.s.beat = 0;', player)
        for forbidden in ('log_line', 'open_file', 'post_load', 'pool_alloc'):
            self.assertNotIn(forbidden, live)
        values = re.search(r'void live_values\(u32\* v, const Edit& e, u32 a, u32 b, bool slice, bool preview = false\) \{(.*?)\n\}',
                           studio, re.S).group(1)
        self.assertIn('normalise_gain(marker_peak(e.start, e.end)', values)    # as render_first measures it
        self.assertIn('const u32 n = tape_frames(b - a, s.stretch);', values)
        self.assertIn('bool live_ok(const Edit& e) { return !(e.beat && dsp::time_active(settings_of(e))); }', studio)
        self.assertIn('if (!preview_render(studio.edit) || studio.rendered == studio.change) return;', studio)
        # the P9 parameter indices on both boards
        names = re.search(r'enum : u32 \{ (kPBegin.*?kParams) \};', player, re.S).group(1)
        self.assertEqual(len([x for x in names.replace('\n', ' ').split(',') if x.strip()]) - 1, 19)
        self.assertIn('const u32 kLiveParams = 19;', studio)

    def test_the_unrolled_drive_division_is_the_long_division(self):
        # studio_dsp.hpp's branch-free step against the plain long-division loop, bit for bit
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('r -= dn & (0u - bit);', dsp)
        self.assertIn('q = (q << 1) | bit;', dsp)
        rnd = random.Random(16)
        M = 0xffffffff
        for _ in range(20000):
            dn = rnd.randint(27 << 22, (27 << 22) + 9 * ((9 << 25) >> 3))
            r0 = rnd.randint(0, dn - 1)
            r, q = r0, 0
            for _ in range(20):
                r = (r << 1) & M
                q = (q << 1) & M
                if r >= dn:
                    r -= dn
                    q |= 1
            r2, q2 = r0, 0
            for _ in range(20):
                r2 = (r2 << 1) & M
                bit = 1 if r2 >= dn else 0
                r2 = (r2 - (dn & ((0 - bit) & M))) & M
                q2 = ((q2 << 1) | bit) & M
            self.assertEqual((q, r), (q2, r2))
            self.assertEqual(q, (r0 << 20) // dn)


class ChainFixes(unittest.TestCase):
    """Filter, bits, playhead and comp: each failure reproduced on the naive arithmetic, then shown gone on the chain's."""
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REFERENCE))
        import studio_chain
        cls.C = studio_chain

    @staticmethod
    def signal(n, amp, decay=0.0, seed=1):
        rnd = random.Random(seed)
        out = []
        for i in range(n):
            a = amp * math.exp(-decay * i / 44100)
            v = a * (0.6 * math.sin(i * 2 * math.pi * 110 / 44100) + 0.3 * math.sin(i * 2 * math.pi * 1870 / 44100)
                     + rnd.uniform(-0.05, 0.05))
            x = int(max(-32768, min(32767, v)))
            out.append((x, int(x * 0.9)))
        return out

    def chain(self, data, **kw):
        C = self.C
        st = C.Settings(begin=0, end=len(data), beat=0, **kw)
        p = C.Pipeline(data, 2, 44100, st)
        out = []
        while True:
            w = p.next()
            if w is None:
                return out
            out.append(w)

    @staticmethod
    def spread(words):
        mean = sum(words) / len(words)
        return math.sqrt(sum((x - mean) ** 2 for x in words) / len(words))

    def test_filter_at_full_resonance_and_its_sweep_never_latch(self):
        C = self.C
        # an unclamped state-variable filter (cutoff index 48, 12 kHz; resonance 100) on loud input
        f, half = C.CUTOFF_Q31[47], 0x7FFFFFFF - (0x79999999 // 100) * 100
        low = band = 0
        stuck = 0
        for x, _ in self.signal(22050, 30000):
            w = x << 16
            for _ in range(2):
                low = C.sat(low + C.mul31(f, band))
                damp = C.mul31(half, band)
                # -low as the device computes it: -(-2^31) wraps to -2^31, so once pinned it pushes on down
                high = C.sat(C.sat(w + C.wrap(-low)) + C.wrap(-C.sat(damp + damp)))
                band = C.sat(band + C.mul31(f, high))
            stuck += low == C.I32_MIN
        self.assertGreater(stuck, 15000)                       # pinned at -1.0: the sound stops
        # the sweep: every cutoff both ways on full-scale square and loud noise, then silence: it never pins
        square = [(32767, 32767) if (i // 50) % 2 else (-32768, -32768) for i in range(2205)]
        for sweep in list(range(-48, 0, 6)) + [-1, 1] + list(range(6, 49, 6)) + [-48, 48]:
            for data in (square, self.signal(2205, 32767)):
                out = self.chain(data + [(0, 0)] * 8820, sweep=sweep)   # 0.2 s of silence: the 50 Hz ring dies
                live = [w[0] for w in out[:2205]]
                self.assertNotEqual(len(set(live[-500:])), 1, sweep)                    # still moving while fed
                tail = [abs(w[0]) for w in out[-200:]]
                self.assertLess(max(tail), 1 << 20, sweep)                               # and dies away after
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('low[c] = sweep_clamp(sat_add(low[c], mul31(f, band[c])));', dsp)
        self.assertIn('const i32 x = w[c] >> 2;', dsp)

    def test_bits_5_and_4_keep_sounding(self):
        C = self.C
        quiet = self.signal(441000, 3000)                      # 10 s at about -21 dBFS
        decaying = self.signal(441000, 30000, decay=2.0)       # loud, falling to about -95 dB over 10 s
        for b in (5, 6):                                       # 5 and 4 bits
            bits = C.BITS[b]
            step = 1 << (32 - bits)
            # a plain mask: a decaying sample under one step by its third second: one or two values, silence
            old = [((x << 16) & ~(step - 1) & 0xFFFFFFFF) for x, _ in decaying[2 * 44100:3 * 44100]]
            self.assertLessEqual(len(set(old)), 2, bits)
            for data, name in ((quiet, 'quiet'), (decaying, 'decaying')):
                out = self.chain(data, bits_index=b)
                for second in range(10):
                    seg = out[second * 44100:(second + 1) * 44100]
                    src = data[second * 44100:(second + 1) * 44100]
                    level = self.spread([x << 16 for x, _ in src])
                    got = self.spread([w[0] for w in seg])
                    if level > 1 << 18:                         # wherever there's sound to crush, it sounds
                        self.assertGreater(got, level * 0.3, (bits, name, second))
                    mean = sum(w[0] for w in seg) / len(seg)
                    self.assertLess(abs(mean), max(level * 0.2, 1 << 16), (bits, name, second))   # no DC
        # every depth, loud and quiet: no silence where there is sound
        for b in range(1, 9):
            for data in (self.signal(44100, 30000), self.signal(44100, 1500)):
                out = self.chain(data, bits_index=b)
                self.assertGreater(self.spread([w[0] for w in out]), 0.2 * self.spread([x << 16 for x, _ in data]), b)
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('w[c] = static_cast<i32>(static_cast<u32>(sat_add(w[c], half)) & mask);', dsp)
        self.assertIn("if (m > bits_env) bits_env = m;", dsp)

    def test_playhead_holds_on_a_render_under_way(self):
        studio = STUDIO.read_text()
        head = re.search(r'void playhead\(u32 t\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertLess(head.index('if (play.growing) {'), head.index('scale_frames(at, n, studio.preview_frames'))
        self.assertIn('const u32 on_card = busy && render.serial == play.which ? render.synced : at;', head)

    def test_comp_tables_and_gain(self):
        C = self.C
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        def table(name):
            body = re.search(name + r'\[101\] = \{([^}]*)\}', dsp, re.S).group(1)
            return [int(v.strip().rstrip('u')) for v in body.replace('\n', ' ').split(',') if v.strip()]
        self.assertEqual(table('kCompThreshold'), C.COMP_THRESHOLD)
        self.assertEqual(table('kCompInverse'), C.COMP_INVERSE)
        self.assertEqual(table('kCompMakeup'), C.COMP_MAKEUP)
        self.assertLess(max(C.COMP_MAKEUP), 1 << 31)
        # a loud steady tone comes out near full scale, a quiet one louder: compressed, made up
        loud = self.chain(self.signal(22050, 30000), comp=100)
        quiet = self.chain(self.signal(22050, 3000), comp=100)
        a = max(abs(w[0]) for w in loud[11025:])
        b = max(abs(w[0]) for w in quiet[11025:])
        self.assertGreater(a, 0.5 * (1 << 31))
        self.assertGreater(b / a, 3000 / 30000 * 2)   # the 20 dB gap is squeezed


class BlockForm(unittest.TestCase):
    """The block form and the cheaper drive, against the frame-by-frame chain."""
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(REFERENCE))
        import studio_chain
        cls.C = studio_chain

    def tone(self, n, channels):
        return [tuple(max(-32768, min(32767, int(20000 * math.sin(i * (0.011 + 0.006 * c)) + 9000 * math.sin(i * 0.37))))
                      for c in range(channels)) for i in range(n)]

    def test_the_block_form_makes_the_same_words(self):
        C = self.C
        for channels in (1, 2):
            data = self.tone(3000, channels)
            for kw in (dict(), dict(drive=100), dict(bits_index=6), dict(rate_index=7), dict(sweep=-40), dict(sweep=20),
                       dict(comp=90), dict(dc=1), dict(stretch=40), dict(stretch=250, reverse=1),
                       dict(drive=70, bits_index=5, rate_index=4, sweep=-12, comp=60, dc=1, stretch=130)):
                st = dict(begin=100, end=2900, beat=0, **kw)
                frame = C.Pipeline(data, channels, 44100, C.Settings(**st))
                want = []
                while True:
                    w = frame.next()
                    if w is None:
                        break
                    want.append((w[0], w[1] if channels > 1 else w[0]))
                block = C.Pipeline(data, channels, 44100, C.Settings(**st))
                got = []
                while True:
                    l, r = block.fill(64)
                    got += list(zip(l, r))
                    if len(l) < 64:
                        break
                if channels < 2:
                    got = [(a, a) for a, _ in got]
                self.assertEqual(got, want, (channels, kw))
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertIn('u32 fill(i32* l, i32* r, u32 n) {', dsp)
        self.assertIn('if (color_active(s)) color_block(l, r, k);', dsp)

    def test_the_drive_reciprocal_keeps_the_curve(self):
        C = self.C
        def old(x, drive):
            v = C.mul31(x, C.Q28 + C.DRIVE_GAIN_STEP * drive)
            three = 3 << 28
            if v >= three:
                t = C.I32_MAX
            elif v <= -three:
                t = -C.I32_MAX
            else:
                v2 = C.mul31(v, v)
                n = C.mul31(v, (27 << 25) + v2)
                dn = (27 << 22) + 9 * (v2 >> 3)
                r, q = abs(n), 0
                if r >= dn:
                    r = dn - 1
                for _ in range(20):
                    r <<= 1
                    q <<= 1
                    if r >= dn:
                        r -= dn
                        q |= 1
                m = q << 11
                t = -m if n < 0 else m
            return C.mul31(t, C.I32_MAX - C.DRIVE_MAKEUP_STEP * drive)
        worst = 0
        for drive in (0, 1, 30, 70, 100):
            for i in range(-4096, 4096):
                x = i << 19
                worst = max(worst, abs(C.drive_word(x, drive) - old(x, drive)))
        self.assertEqual(worst, 0)                 # the long division's curve exactly: the remainder makes the quotient exact
        rnd = random.Random(18)
        M32 = 0xFFFFFFFF
        for _ in range(200000):                    # the quotient alone, all over dn's range, edges included
            dn = rnd.choice([27 << 22, (108 << 22) - 1, rnd.randint(27 << 22, (108 << 22) - 1)])
            r = rnd.choice([0, dn - 1, rnd.randint(0, dn - 1)])
            at = dn - C.DRIVE_RECIP_BASE
            high, low = C.DRIVE_RECIP[at >> 20], C.DRIVE_RECIP[(at >> 20) + 1]
            recip = high - C.mul31(high - low, (at & 0xFFFFF) << 11)
            p = C.mul31(C.wrap(dn << 2), recip)
            recip = C.wrap(C.mul31(recip, C.wrap(((1 << 29) - p) & M32)) << 3)
            q = (C.mul31(C.wrap(r << 2), recip) & M32) >> 8
            self.assertLessEqual(abs(q - (r << 20) // dn), 1, (r, dn))     # within one before the correction
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        self.assertNotIn('OPXY_DRIVE_STEP', dsp)   # no per-sample long division left in the drive
        self.assertIn('const u32 kDriveRecip[326] = {', dsp)
        body = re.search(r'const u32 kDriveRecip\[326\] = \{([^}]*)\}', dsp, re.S).group(1)
        self.assertEqual([int(v.strip().rstrip('u')) for v in body.replace('\n', ' ').split(',') if v.strip()], C.DRIVE_RECIP)


class BounceReview(unittest.TestCase):
    """Review findings on the real-time bounce, pinned."""
    def test_a_finished_bounce_never_holds_the_next_play(self):
        player = PLAYER.read_text()
        free = re.search(r'void bounce_free\(\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertLess(free.index('bounce_ready = false;'), free.index('bounce_capturing = false;'))   # ready first
        self.assertIn('if (bounce_serial) bounce_done = bounce_serial;', free)                     # retired, not forgotten
        self.assertIn('if (bounce_want && bounce_want != bounce_done && !(bounce_want == bounce_serial && bounce_ready)) return false;',
                      player)
        self.assertIn('if (bounce_want && bounce_want != bounce_serial && bounce_want != bounce_done) {', player)
        self.assertIn('    if (!out) return;', player)                                              # capture_block
        self.assertIn('else if (++player.wait_blocks > 1400u) player.waiting = false;', player)
        self.assertIn('if (job_pending && blocks - job_posted_block < 1400u) return;', player)
        self.assertIn('if (!staged_live.s.stretch) staged_live.s.stretch = 100;', player)

    def test_the_bounce_size_and_order(self):
        studio = STUDIO.read_text()
        self.assertIn('const u32 out = n / rate * 44100u + n % rate * 44100u / rate;', studio)
        for n, rate in ((882000, 48000), (3528000, 48000), (100, 22050), (4000000, 96000)):
            total = n // rate * 44100 + n % rate * 44100 // rate
            self.assertLess(n % rate * 44100, 1 << 32)
            self.assertLessEqual(abs(total - n * 44100 // rate), 1)
        done = re.search(r'void bounce_prepare_done\(u32\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertLess(done.index('b3(151'), done.index('b3(150'))                      # the size before the arm
        start = re.search(r'bool start_bounce\(bool slices = false\) \{(.*?)\n\}', studio, re.S).group(1)
        self.assertNotIn('choose_paths()', start)                                          # file work on the worker
        self.assertIn('queue(bounce_prepare_job, bounce_prepare_done);', start)
        self.assertIn('end_bounce("print stopped", "nothing saved");', studio)               # Sample stops a print


class EditLabel(unittest.TestCase):
    """On a drum sampler or multisampler, "shift: edit" would sit over stock's arrow on M1. The label
    shows there only while Shift is held, and never reads "shift: edit"."""

    def test_label_only_with_shift_on_drum_and_multi(self):
        text = STUDIO.read_text()
        body = re.search(r'void label\(\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertNotIn('"shift: edit"', body)
        self.assertIn('if (!needs_shift || host::shift_held()) m_label(0, "edit");', body)
        self.assertEqual(body.count('m_label('), 1)


class HighlightAndFilter(unittest.TestCase):
    """The highlighted file on drum and multi, the filter's direction and readouts, the waveform's
    seams, the retime render kept under live colour, and each pad playing its slice."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_shift_edit_opens_the_highlighted_file_for_the_key(self):
        body = self.body('bool open_from_library(bool shift) {')
        self.assertIn('if (engine != kSampler && highlighted(path, sizeof path) && !same_text(path, own, kPathBytes))\n'
                      '            return open_file(track, path, engine, slot);', body)
        saved = self.body('void load_saved() {')
        self.assertIn('if (studio.file_only && !studio.on_key) {', saved)
        self.assertIn('const unsigned char* before = studio.on_key ? studio.key_entry : studio.origin;', saved)
        self.assertIn('studio.file_only = studio.on_key = false;', saved)
        kit = self.body('void load_kit() {')
        self.assertIn('studio.on_key ? studio.key_entry : studio.origin', kit)

    def test_sweep_knob_lowers_the_low_pass_to_the_left(self):
        text = STUDIO.read_text()
        self.assertIn('i32 sweep_place(i32 v) { return v < 0 ? -(static_cast<i32>(dsp::kCutoffCount) + 1 + v) : v; }', text)
        place = lambda v: -(48 + 1 + v) if v < 0 else v
        sweeps = [place(p) for p in range(-48, 49)]
        self.assertEqual(sorted(sweeps), list(range(-48, 49)))          # a bijection, its own inverse
        self.assertTrue(all(place(place(p)) == p for p in range(-48, 49)))
        cutoff = lambda sw: abs(sw)                                     # kCutoffHz index + 1, rising
        lows = [cutoff(place(p)) for p in range(-1, -49, -1)]          # one step left of off, then further
        self.assertEqual(lows, sorted(lows, reverse=True))             # further left: lower
        self.assertEqual(lows[0], 48)                                   # first: 12 kHz
        self.assertIn('append(values[3], 12, n, e.sweep < 0 ? "-" : "+");', text)

    def test_readouts_fit_five_characters(self):
        def secs(ms):
            out = str(ms // 1000)
            if ms >= 100000:
                return out
            frac = ms % 1000
            d = '.' + str(frac // 100) + str(frac // 10 % 10)
            return out + (d[:2] if ms >= 10000 else d)
        def fade(ms):
            return f'{ms}ms' if ms < 1000 else secs(ms) + 's'
        for ms in (0, 9, 999, 1000, 1234, 9999, 10000, 59999, 99999, 100000, 600000):
            self.assertLessEqual(len(secs(ms) + 's'), 5, ms)
        for ms in range(0, 2001, 5):
            self.assertLessEqual(len(fade(ms)), 5, ms)
        self.assertEqual(secs(1234), '1.23')
        self.assertEqual(secs(12345), '12.3')

    def test_waveform_edges_have_no_seam(self):
        text = STUDIO.read_text()
        self.assertNotIn('taper', text)
        self.assertNotIn('kSeamAmp', text)
        self.assertNotIn('ribbon(panel::N_DIM0', text)
        self.assertNotIn('ribbon(panel::N_LIT0', text)
        self.assertNotIn('show(panel::N_LIT0', text)
        self.assertNotIn('show(panel::N_DIM0', text)
        hpp = PANEL_HPP.read_text()
        self.assertIn('const u32 N_DIMLO0 = 8;', hpp)
        self.assertIn('const u32 N_LITLO0 = 12;', hpp)
        self.assertIn('const i32 kEdge = 512;', text)   # half a pixel: closing edges over the 1/4 px limit

    def test_retime_render_keeps_under_live_changes(self):
        text = STUDIO.read_text()
        touched = self.body('void touched() {')
        self.assertIn('same_time(studio.edit, studio.time_last)', touched)
        self.assertIn('if (!keep) {\n        ++studio.change;', touched)
        self.assertIn('render.edit = target == kToPreview && !live_ok(e) ? time_only(e) : e;', text)
        only = self.body('Edit time_only(const Edit& e) {')
        for field in ('level', 'normal', 'dc', 'fade_in', 'fade_out', 'chans', 'rate_index', 'bits_index', 'drive',
                      'sweep', 'comp', 'send1', 'send2'):
            self.assertRegex(only, rf'\bt\.{field}\b', field)
        same = self.body('bool same_time(const Edit& a, const Edit& b) {')
        for field in ('start', 'end', 'reverse', 'stretch', 'pitch', 'beat', 'grain'):
            self.assertIn(f'a.{field} == b.{field}', same)
        edit = self.body('void play_edit() {')
        self.assertIn('play_live_preview(a, b,', edit)
        self.assertIn('play.mode = 4;', self.body('void play_live_preview(u32 a, u32 b, bool slice) {'))
        player = PLAYER.read_text()
        self.assertIn('kLiveFile = 4', player)
        self.assertIn('if (load_serial != c.which || !pcm || loaded_frames < l.s.end) return false;', player)
        self.assertIn('player.from_file = c.mode == kFromFile || c.mode == kLiveFile;', player)

    def test_kit_slices_and_empty_drum_keys_are_neutral(self):
        assign = self.body('bool assign_file(u32 track, u32 engine, u32 slot, const char* path, u32 frames, u32 root) {')
        self.assertIn('return host::put_file(track, engine, slot, path, frames, root);', assign)
        self.assertIn('if (entry && !entry_root(entry) && !entry_fine(entry)) set_entry_tuning(entry, 60, 0);', HOST.read_text())
        kit = self.body('void load_kit() {')
        self.assertIn('host::set_entry_tuning(placed, 60, 0);', kit)
        self.assertIn('const i32 kNeutralRootCents = 6000;', (PLATFORM / 'stock_sample.hpp').read_text())

    def test_each_pad_plays_its_slice(self):
        follow = self.body('void follow_notes(u32 t) {')
        self.assertIn('const u32 slice = struck >= kKitFirstKey ? struck - kKitFirstKey + 1 : 0;', follow)
        self.assertIn('if (studio.page == kChop && slice && slice <= studio.cut_count) {', follow)
        self.assertIn('studio.edit.slice = kept;', follow)


class NewKit(unittest.TestCase):
    """A30: the slices as a new drum kit preset: stock's empty kit with regions as stock's kit zebra.json has them."""
    # an empty user kit pulled from a device (its /presets/snapshot folder), when STUDIO_EMPTY_KIT names one
    EMPTY_KIT = Path(os.environ.get('STUDIO_EMPTY_KIT', '/nonexistent'))

    # zebra.json's keys, as stock writes them (sorted); its regions carry "gain" only when not 0
    TOP = ['engine', 'envelope', 'fx', 'lfo', 'octave', 'platform', 'regions', 'type', 'version']
    REGION = ['fade.in', 'fade.out', 'framecount', 'hikey', 'lokey', 'pan', 'pitch.keycenter', 'playmode',
              'reverse', 'sample', 'transpose', 'tune']

    def constant(self, text, name):
        block = re.search(r'const char ' + name + r'\[\] =(.*?);\n', text, re.S).group(1)
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"', block)
        return ''.join(p.encode().decode('unicode_escape') for p in parts)

    def patch(self, names):
        text = STUDIO.read_text()
        c = {n: self.constant(text, n) for n in
             ('kKitHead', 'kKitRegionA', 'kKitRegionB', 'kKitRegionC', 'kKitRegionD', 'kKitTail')}
        first = int(re.search(r'const u32 kKitFirstKey = (\d+);', text).group(1))
        escape = lambda s: ''.join('\\' + ch if ch in '"\\' else ch for ch in s if ord(ch) >= 0x20)
        regions = ','.join(c['kKitRegionA'] + str(first + k) + c['kKitRegionB'] + str(first + k) +
                           c['kKitRegionC'] + escape(n) + c['kKitRegionD'] for k, n in enumerate(names))
        return c['kKitHead'] + regions + c['kKitTail']

    def test_the_patch_is_stock_kit_json(self):
        names = ['break-s%d.wav' % (k + 1) for k in range(24)]
        doc = json.loads(self.patch(names))
        self.assertEqual(list(doc), self.TOP)
        self.assertEqual((doc['type'], doc['version'], doc['platform']), ('drum', 4, 'OP-XY'))
        self.assertEqual(len(doc['regions']), 24)
        for k, r in enumerate(doc['regions']):
            self.assertEqual(list(r), self.REGION)
            self.assertEqual((r['lokey'], r['hikey']), (53 + k, 53 + k))   # zebra's keys 53..76
            self.assertEqual(r['sample'], names[k])
            self.assertEqual((r['playmode'], r['pitch.keycenter'], r['framecount']), ('oneshot', 60, 0))

    def test_odd_names_stay_valid_json(self):
        names = ['a "quoted" \\ name-s1.wav', 'tab\there-s2.wav']
        doc = json.loads(self.patch(names))
        self.assertEqual(doc['regions'][0]['sample'], names[0])
        self.assertEqual(doc['regions'][1]['sample'], 'tabhere-s2.wav')   # control characters dropped

    def test_the_patch_is_written_last_and_the_card_offers_it(self):
        text = STUDIO.read_text()
        last = re.search(r'void render_last\(u32\) \{(.*?)\n\}', text, re.S).group(1)
        self.assertIn('if (render.target == kToSlices && render.new_kit) {', last)
        self.assertIn('fail("couldn\'t write the kit");', last)                 # the cleanup removes the slices
        self.assertIn('if (!ok) remove_file(path);', text)                       # no half patch
        self.assertIn('if (render.new_kit) return choose_kit_paths(stem);', text)
        self.assertIn('render.new_kit = target == kToSlices && slices_new_kit;', text)
        self.assertIn('m_label(1, "new kit");', text)
        self.assertIn('if (studio.engine == kDrumSampler) m_label(3, "this kit");', text)
        self.assertIn('(key == kKeyM4 && studio.engine == kDrumSampler)', text)
        self.assertNotIn('if (studio.engine == kDrumSampler && !slice_choice_made)', text)   # every track asks
        for card in ('"new kit, files, kit"', '"new kit or files"', '"new kit saved"'):
            self.assertLess(len(card) - 2, 24, card)                               # fits a card line

    def test_no_slices_is_stocks_own_empty_kit(self):
        # an empty user kit, as stock writes it (/presets/snapshot/<date> (n).preset/patch.json)
        found = sorted(self.EMPTY_KIT.glob('*.preset/patch.json'))
        if not found:
            self.skipTest('STUDIO_EMPTY_KIT is not set to an empty kit pulled from a device')
        self.assertEqual(self.patch([]), found[0].read_text())                   # byte for byte

    def test_paths_fit(self):
        # kPresetFolder/<16-char stem>-20.preset/<stem>-s24.wav within slice_path's 95
        folder = '/fat32/presets/snapshot/' + 's' * 16 + '-20.preset'
        self.assertLess(len(folder), 96)                                          # kPathBytes
        self.assertLessEqual(len(folder + '/' + 's' * 16 + '-s24.wav'), 95)
        self.assertIn('kPresetFolder[] = "/fat32/presets/snapshot";', STUDIO.read_text())


class Relist(unittest.TestCase):
    """New kit (and every save) is listed in the library without a restart."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_relist_does_what_stock_does_after_its_own_save(self):
        self.assertIn('library_stale = false;', self.body('void relist_library() {'))
        self.assertIn('host::relist_library();', self.body('void relist_library() {'))
        body = re.search(r'\nvoid relist_library\(\) \{(.*?)\n\}', HOST.read_text(), re.S).group(1)
        self.assertIn('const u32 tree = stock<u32()>(0x8c0ad5dc)();', body)
        self.assertIn('stock<void(u32)>(0x8c0a7e70)(tree);', body)
        # entering the library queues its listing step this way (8c0a2ee4..8c0a2ef8)
        self.assertIn('stock<void(u32, u32, u32, u32)>(0x8d4dd728)(0, 0, 0x8c0a349cu, 0);', body)
        self.assertLess(body.index('0x8c0a7e70'), body.index('0x8d4dd728'))   # clear the flag, then list

    def test_saves_mark_the_library_and_close_lists_it(self):
        text = STUDIO.read_text()
        self.assertEqual(text.count('files_written();'), 2)   # full render and real-time print
        for saved in ('"saved %s -> %s: %d frames, %d ch -> %d, %d Hz (full render',
                      '"saved %s -> %s: %d frames, 2 ch, 44100 Hz (printed in real time'):
            at = text.index(saved)
            self.assertIn('files_written();', text[at - 120:at])
        written = self.body('void files_written() {')
        self.assertIn('library_stale = true;', written)
        self.assertIn('if (studio_closed()) relist_library();', written)
        close = self.body('void close_studio(bool keep) {')
        self.assertIn('if (library_stale) relist_library();', close)
        self.assertLess(close.index('drop_pcm();'), close.index('relist_library'))
        self.assertIn('bool studio_closed() { return studio.mode == kClosed; }', text)

    def test_decision_recorded(self):
        self.assertIn('// A35 The library lists saves at once', STUDIO.read_text())


class PresetIndex(unittest.TestCase):
    """New kit shows in presets > snapshot (A37); every pad plays its own slice (A36)."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_new_kit_builds_stocks_preset_index_again(self):
        body = self.body('void rescan_presets() {')
        self.assertIn('presets_stale = false;', body)
        self.assertIn('host::rescan_presets();', body)
        self.assertIn('stock<void(u32)>(0x8d4eefec)(0);', HOST.read_text())   # 0, as rename and delete pass; boot passes 1
        kit = self.body('void load_kit() {')
        self.assertIn('presets_stale = true;', kit)
        self.assertIn('if (studio_closed()) rescan_presets();', kit)
        self.assertLess(kit.index('set_card("new kit saved"'), kit.index('presets_stale = true;'))
        close = self.body('void close_studio(bool keep) {')
        self.assertIn('if (presets_stale) rescan_presets();', close)

    def test_only_a_new_kit_rescans(self):
        text = STUDIO.read_text()
        self.assertEqual(text.count('presets_stale = true;'), 1)
        self.assertEqual(text.count('host::rescan_presets();'), 1)
        self.assertEqual(HOST.read_text().count('0x8d4eefec'), 1)

    def test_a_live_slice_plays_its_source_frames(self):
        # A36: cuts are output frames; with tape retime the output is longer than the source, so the live
        # slice maps them back (unmapped, with 24 slices at x1.72 pad 14 begins past the source end and is silent)
        body = self.body('void slice_source(const Edit& e, u32 k, u32& a, u32& b) {')
        self.assertIn('const u32 src_len = e.end - e.start, n = output_length();', body)
        self.assertIn('scale_frames(studio.cuts[k - 1], src_len, n)', body)
        self.assertIn('if (s1 > src_len) s1 = src_len;', body)
        play = self.body('void play_edit() {')
        self.assertEqual(play.count('slice_source(e, e.slice, a, b);   // A36'), 2)
        self.assertNotIn('e.start + s0', play)
        # 24 slices at x1.72: source 4222..86391, output 143795, 24 even cuts; every slice inside the source
        src, n = 86391 - 4222, 143795
        cuts = [n * k // 24 for k in range(25)]
        for k in range(1, 25):
            a = 4222 + scale_frames(cuts[k - 1], src, n)
            b = 4222 + min(scale_frames(cuts[k], src, n), src)
            self.assertTrue(4222 <= a < b <= 86391, k)
        self.assertLessEqual(abs((4222 + scale_frames(cuts[14 - 1], src, n)) - (4222 + src * 13 // 24)), 2)


class GrowingPreview(unittest.TestCase):
    """A beat retime played while it renders keeps sounding once fully rendered (A38, P13)."""

    def test_a_stop_keeps_growing(self):
        stop = re.search(r'void stop_play\(\) \{(.*?)\n\}', STUDIO.read_text(), re.S).group(1)
        self.assertLess(stop.index('player_send();'), stop.index('play.growing = false;'))

    def test_the_loader_carries_on_to_a_larger_count(self):
        player = PLAYER.read_text()
        self.assertIn('want_resume = true;   // P13', player)
        work = re.search(r'void load_work\(\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('load_done && segmented && !file && loaded_frames < want_limit && loaded_frames < kMaxFrames &&', work)
        self.assertIn('!(loaded_frames % kSegmentFrames)', work)
        self.assertLess(work.index('if (want_resume) {'), work.index('if (!pcm || !load_serial || load_done) return;'))
        self.assertIn('want_resume = false;\n', work[work.index('load_serial = want_serial;'):])


class RetimeSearch(unittest.TestCase):
    """The WSOLA search's probes, made once a hop, pick the same splice as a per-candidate search."""

    @staticmethod
    def old_search(probe, nominal, natural, hop, length):
        span = min(hop // 2, 256)
        best, best_score = nominal, -0x7FFFFFFF
        for d in range(-span, span + 1, 8):
            c = nominal + d
            if c < 0 or c + hop > length:
                continue
            score = 0
            for j in range(0, hop, 16):
                score = (score + probe(c + j) * probe(natural + j) + 2**31) % 2**32 - 2**31   # i32
            if score > best_score:
                best_score, best = score, c
        return best

    @staticmethod
    def grid_search(probe, nominal, natural, hop, length):
        # studio_dsp.hpp beat_frame, line for line
        span = min(hop // 2, 256)
        best, best_score = nominal, -0x7FFFFFFF
        terms = (hop + 15) // 16
        candidates = 2 * span // 8 + 1
        slots = candidates + 2 * (terms - 1)
        assert terms <= 240 and slots <= 544
        base = nominal - span
        natural_probes = [probe(natural + 16 * m) for m in range(terms)]
        grid = [probe(base + 8 * t) if 0 <= base + 8 * t < length else 0 for t in range(slots)]
        for k in range(candidates):
            c = base + 8 * k
            if c < 0 or c + hop > length:
                continue
            score = 0
            for m in range(terms):
                score = (score + grid[k + 2 * m] * natural_probes[m] + 2**31) % 2**32 - 2**31
            if score > best_score:
                best_score, best = score, c
        return best

    def test_same_splice(self):
        rng = random.Random(24)
        for trial in range(300):
            hop = rng.choice([64, 128, 441, 882, 1323, 3840, rng.randrange(32, 3841)])
            length = rng.randrange(2 * hop + 1, 2 * hop + 40000)
            values = [rng.randrange(-4096, 4096) for _ in range(length)]
            calls = []

            def probe(i):
                assert 0 <= i < length, i   # never outside the source
                calls.append(i)
                return values[i]
            natural = rng.randrange(0, length - hop + 1)
            nominal = rng.randrange(0, length - hop + 1)
            self.assertEqual(self.grid_search(probe, nominal, natural, hop, length),
                             self.old_search(probe, nominal, natural, hop, length), (trial, hop, nominal))

    def test_the_cpp_is_this_search(self):
        dsp = (EXT / 'studio_dsp.hpp').read_text()
        for line in ('const u32 terms = (hop + 15) / 16;', 'const u32 candidates = 2 * span / 8 + 1;',
                     'const u32 slots = candidates + 2 * (terms - 1);',
                     'const i32 base = static_cast<i32>(nominal) - static_cast<i32>(span);',
                     'for (u32 m = 0; m < terms; ++m) natural_probes[m] = probe(natural + 16 * m);',
                     'for (u32 m = 0; m < terms; ++m) score += grid_probes[k + 2 * m] * natural_probes[m];',
                     'static constexpr u32 kProbeTerms = 240, kProbeSlots = 544;'):
            self.assertIn(line, dsp)


class OnePassSaves(unittest.TestCase):
    """Slice saves in one pass (A40), saves from the finished preview (A41), smaller kits (A42), and the
    studio staying on the source after this kit (A43)."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_slices_of_a_retime_run_once(self):
        first = self.body('void render_first(u32) {')
        self.assertIn('render.split = render.target == kToSlices && render.counting;   // A40', first)
        self.assertIn('render.chans = kKeep;', first)
        self.assertIn('render.out_channels = render.channels;', first)
        write = self.body('void begin_write() {')
        self.assertIn('if (render.split) render.fi = render.fo = 0;', write)
        self.assertIn('open_output(render.in_path, render.estimate);', write)
        split = self.body('void begin_split() {')
        for line in ('render.in_total = render.length;', 'render.in_channels = render.out_channels;',
                     'render.chans = render.split_chans;', 'render.out_channels = render.split_out;',
                     'begin_slices(render.length, render.length);'):
            self.assertIn(line, split)
        self.assertLess(split.index('render.in_channels = render.out_channels;'),
                        split.index('render.out_channels = render.split_out;'))
        last = self.body('void render_last(u32) {')
        self.assertTrue(last.lstrip().startswith('if (render.split && !render.splitting) {'))
        finish = self.body('void render_finish(bool keep) {')
        self.assertIn('if (render.split && render.in_path[0]) remove_file(render.in_path);', finish)
        step = self.body('void render_step() {')
        self.assertIn('if (render.target == kToSlices && (!render.split || render.splitting)) {', step)

    def test_the_reader_reads_in_order(self):
        text = STUDIO.read_text()
        ready = self.body('bool in_ready(u32 pos) {')
        self.assertIn('if (pos != render.in_start + render.in_frames || pos >= render.in_total)', ready)
        self.assertIn('if (n > kInFrames) n = kInFrames;', ready)
        opened = self.body('bool in_open(u32 pos) {')
        self.assertIn('segment_path(path, sizeof path, render.in_path, k);', opened)
        self.assertIn('file_read(skip, 1, kHeaderBytes, render.in_file) != kHeaderBytes', opened)
        self.assertIn('return stock<u32(void*, u32, u32, File*)>(0x8fed05bc)(to, size, count, file);', HOST.read_text())
        # a model of the reader over segments: every frame once, in order, from the right file
        total, seg = 200000, 65536
        frames, start, count, file_end, opened_at = [], 0, 0, 0, []
        for pos in range(total):
            if not (start <= pos < start + count):
                self.assertEqual(pos, start + count)
                if pos >= file_end:
                    k = pos // seg
                    self.assertEqual(pos, k * seg)
                    opened_at.append(pos)
                    file_end = (k + 1) * seg if total - k * seg > seg else total
                count = min(file_end - pos, 8192)
                start = pos
            frames.append(pos)
        self.assertEqual(frames, list(range(total)))
        self.assertEqual(opened_at, [0, 65536, 131072, 196608])

    def test_a_save_reads_the_finished_preview(self):
        render = self.body('void start_render(Target target, bool quick = false) {')
        self.assertIn('render.from_preview = target != kToPreview && !live_ok(e) && !e.normal && studio.out_valid &&', render)
        self.assertIn('studio.rendered == studio.change && studio.showing_preview && studio.shown_segmented &&', render)
        self.assertIn('!studio.cut_at_limit && studio.preview_frames && studio.preview_channels;', render)
        pipe = self.body('void start_pipe() {')
        for line in ('t.start = 0;', 't.end = render.in_total;', 't.reverse = 0;', 't.stretch = 100;', 't.pitch = 0;',
                     't.beat = 0;', 'render.pipe.start(render.in_buf, render.in_channels, render.rate, s);'):
            self.assertIn(line, pipe)
        step = self.body('void render_step() {')
        self.assertIn('if (render.from_preview && render.pipe.read < render.in_total) {', step)
        self.assertIn('render.pipe.data = render.in_buf - render.in_start * render.in_channels;', step)
        first = self.body('void render_first(u32) {')
        self.assertLess(first.index('if (render.from_preview) {'), first.index('borrow(render.key)'))

    def test_this_kit_on_fewer_pads_and_the_studio_stays(self):
        kit = self.body('void load_kit() {')
        self.assertIn('else if (!note_of[at / kEntryBytes]) {', kit)
        self.assertIn('while (count < render.cut_count && first + count < 24 && note_of[first + count]) ++count;', kit)
        self.assertNotIn('copy_text(studio.path, render.slice_paths[0]', kit)
        self.assertNotIn('forget_preview();', kit)
        self.assertIn('log(__LINE__, "kit: the studio stays on %s", studio.path);', kit)


class PrintSends(unittest.TestCase):
    """A44: beat retime prints its sends (reverb included) through its preview; A45: slices print."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_a_beat_retime_prints_through_its_preview(self):
        ready = self.body('bool full_preview_ready() {')
        for part in ('studio.out_valid', 'studio.rendered == studio.change', 'studio.shown_segmented',
                     '!studio.cut_at_limit', 'studio.preview_frames'):
            self.assertIn(part, ready)
        self.assertIn('return (live_ok(e) || full_preview_ready()) && (!studio.file_only || studio.on_key);',
                      self.body('bool can_print(const Edit& e) {'))
        bounce = self.body('bool start_bounce(bool slices = false) {')
        self.assertIn('bounce.from_preview = !live_ok(e);', bounce)
        self.assertIn('const u32 n = bounce.from_preview ? studio.preview_frames : tape_frames(e.end - e.start, e.stretch);', bounce)
        self.assertIn('u32 total = out + 44100u * (bounce.until_quiet ? kFullTailS : e.tail);', bounce)
        done = self.body('void bounce_prepare_done(u32) {')
        self.assertIn('if (bounce.from_preview) play_live_preview(0, studio.preview_frames, false);   // A44', done)
        player = PLAYER.read_text()
        live_file = player[player.index('    if (c.mode == kLiveFile) {'):player.index('    } else if (c.mode == kLive) {')]
        self.assertIn('if (bounce_want && bounce_want != bounce_done && !(bounce_want == bounce_serial && bounce_ready)) return false;', live_file)
        self.assertLess(live_file.index('return false;'), live_file.index('start_live(c)'))
        self.assertIn('if (bounce_ready && !bounce_capturing && !bounce_full) bounce_capturing = true;   // P14', live_file)

    def test_slices_print_then_cut(self):
        save = self.body('void start_save() {')
        self.assertIn('if (wants_bounce(studio.edit) && can_print(studio.edit) && start_bounce(true)) return;', save)
        self.assertLess(save.index('start_bounce(true)'), save.index('start_render(kToSlices);'))
        check = self.body('void bounce_check_job(u32) {')
        self.assertIn('if (bounce.found && bounce.slices) {', check)
        follow = self.body('void follow_bounce(u32 t) {')
        self.assertLess(follow.index('start_print_split();'), follow.index('files_written();'))
        split = self.body('void start_print_split() {')
        for line in ('render.split = render.splitting = true;', 'copy_text(render.in_path, bounce.result, sizeof render.in_path);',
                     'render.in_total = bounce.total;', 'render.in_channels = 2;', 'render.printed = true;',
                     'render.new_kit = slices_new_kit;', 'queue(print_split_first, after_render);'):
            self.assertIn(line, split)
        self.assertIn('begin_slices(print_out_frames, render.in_total);', self.body('void print_split_first(u32) {'))
        slices = self.body('void begin_slices(u32 n, u32 end) {')
        self.assertIn('render.cuts[render.cut_count] = end > render.cuts[render.cut_count - 1] ? end : render.cuts[render.cut_count - 1] + 1;', slices)

    def test_the_tail_goes_on_the_last_slice(self):
        # begin_slices' cuts for a print: the edit's 44.1 kHz length, the last slice to the print's end
        out, total, q16 = 1000000, 1000000 + 44100 * 4, [0, 16384, 32768, 49152, 65536]
        cuts = []
        for k, q in enumerate(q16):
            at = (out >> 16) * q + (((out & 0xFFFF) * q) >> 16)
            at = min(at, out)
            if k and at <= cuts[-1]:
                at = cuts[-1] + 1
            cuts.append(at)
        cuts[0] = 0
        cuts[-1] = total if total > cuts[-2] else cuts[-2] + 1
        self.assertEqual(cuts[1:4], [250000, 500000, 750000])
        self.assertEqual(cuts[-1] - cuts[-2], 250000 + 44100 * 4)


class MirrorAndTail(unittest.TestCase):
    """The trim page flips when reversed (A46); tail "full" until quiet, with a cap (A47, P15)."""

    def body(self, signature):
        return re.search(re.escape(signature) + r'(.*?)\n\}', STUDIO.read_text(), re.S).group(1)

    def test_the_trim_page_mirrors(self):
        text = STUDIO.read_text()
        self.assertIn('bool mirrored() { return studio.page == kTrim && studio.edit.reverse; }', text)
        self.assertIn('i32 x_shown(const View& v, u32 f) { return mirrored() ? q(kX0) + q(kX0 + kWidth) - x_of(v, f) : x_of(v, f); }', text)
        trim = self.body('void trim_stage() {')
        self.assertNotIn('x_of(v,', trim)                     # every position through x_shown
        self.assertIn('const u32 shown = mirror ? 3 - qd : qd;', trim)
        self.assertEqual(trim.count('const u32 k = mirror ? 31 - c : c;'), 2)
        self.assertIn('const u32 first = mirror ? e.end : e.start, last = mirror ? e.start : e.end;', trim)
        self.assertIn('show(panel::N_PLAYHEAD, x_shown(v, src_f), q(kStageMid));', self.body('void playhead(u32 t) {'))
        # the mirror maps the stage onto itself, ends swapped
        X0, W, unit = 4, 312, 1024
        q = lambda px: px * unit
        x_of = lambda f, v0, v1: q(X0) + (f - v0) * W * unit // (v1 - v0)
        shown = lambda f: q(X0) + q(X0 + W) - x_of(f, 0, 1000)
        self.assertEqual(shown(0), q(X0 + W))
        self.assertEqual(shown(1000), q(X0))
        self.assertTrue(all(shown(f) > shown(f + 1) for f in range(1000)))

    def test_full_tail(self):
        text = STUDIO.read_text()
        self.assertIn('const u32 kTailFull = 11, kFullTailS = 30;', text)
        self.assertIn('e.tail = step_by(e.tail, detents, 1, 0, kTailFull);', text)
        self.assertIn('if (e.tail >= kTailFull) append(values[6], 12, n, "full");', text)
        bounce = self.body('bool start_bounce(bool slices = false) {')
        self.assertIn('u32 total = out + 44100u * (bounce.until_quiet ? kFullTailS : e.tail);', bounce)
        self.assertIn('b3(151, static_cast<i32>(bounce.total | (bounce.until_quiet ? 0x80000000u : 0u)));',
                      self.body('void bounce_prepare_done(u32) {'))
        check = self.body('void bounce_check_job(u32) {')
        self.assertLess(check.index('file_read(h, 1, kHeaderBytes, f)'), check.index('rename_file'))
        follow = self.body('void follow_bounce(u32 t) {')
        self.assertIn('bounce.capped = bounce.until_quiet && bounce.got >= bounce.total;', follow)
        self.assertLess(follow.index('bounce.total = bounce.got;'), follow.index('start_print_split();'))
        self.assertIn('set_card("tail cut at 30 s", "still ringing", 5000);', follow)
        player = PLAYER.read_text()
        self.assertIn('bounce_quiet_want = (v & 0x80000000u) != 0;', player)
        self.assertIn('const u32 frames = v & 0x7fffffffu;', player)
        self.assertIn('const u32 kQuietHold = 2 * 44100, kQuietKeep = 4410;', player)
        capture = re.search(r'void capture_block\(\) \{(.*?)\n\}', player, re.S).group(1)
        self.assertIn('if (bounce_until_quiet && !bounce_play_ended && !player.active) {', capture)
        self.assertLess(capture.index('bounce_total = keep < at ? keep : at;'), capture.index('if (at >= bounce_total) {'))

    def test_a_quiet_model(self):
        # P15 on a decaying tail: stops 2 s after the last frame over 32, keeping 0.1 s past it
        level, hold, keep = 32, 88200, 4410
        play_end, total = 44100, 44100 + 30 * 44100
        loud_until = play_end + 3 * 44100                     # rings 3 s past the edit
        frames = [1000 if f < loud_until else 5 for f in range(total)]
        at, loud_at, ended, out_total = 0, 0, False, total
        while at < out_total:
            if not ended and at >= play_end:
                ended, loud_at = True, at
            listen = ended
            for _ in range(64):
                if at >= out_total:
                    break
                if listen and abs(frames[at]) > level:
                    loud_at = at
                at += 1
            if listen and at < out_total and at - loud_at >= hold:
                out_total = min(loud_at + keep, at)
        self.assertEqual(out_total, loud_until - 1 + keep)


class PrintCard(unittest.TestCase):
    """The print card's second line fits its 23 characters at every length a print can have."""

    def test_the_card_line_fits(self):
        text = STUDIO.read_text()
        self.assertIn('append(line, sizeof line, m, bounce.until_quiet ? " s max; sample stops" : " s; sample stops");', text)
        for seconds in (1, 9, 35, 91):
            for tail in (" s max; sample stops", " s; sample stops"):
                self.assertLessEqual(len(str(seconds) + tail), 23)
