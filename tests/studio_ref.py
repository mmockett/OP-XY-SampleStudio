#!/usr/bin/env python3
"""Sample Studio references: the exact output each tool must produce.

The device code (sdk/platform/opxy-1.2.1/sample_studio*) must match these to the sample, so a
file bounced on the OP-XY nulls against the same edit made here. Everything is
integer arithmetic the Blackfin can copy without 64-bit multiplies or library
division: samples travel as Q31 words (16-bit PCM shifted up 16), gains are
unsigned Q4.28, and 16-bit output uses bounce phase 1's TPDF dither (open-xy
local/bounce-phase1, bounce.hpp `Dither`).

A file that no stage changed is copied, not dithered: crop and reverse alone
are bit-exact.

Usage:
    python3 studio_ref.py in.wav out.wav [--start F] [--end F] [--trim DB]
        [--reverse] [--dc] [--gain DB | --normalise DB] [--fade-in MS] [--fade-out MS]
        [--root NOTE] [--seed N]
    python3 studio_ref.py --name samples/user/kick.wav     # the next free -edN name
"""
import argparse
import math
import os
import re
import struct
import sys

Q28 = 1 << 28
I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
PATH_MAX = 95                 # stock region paths hold at most 95 characters
LOADER_FRAMES = 882000        # stock decodes at most 20 s at 44.1 kHz
NAME_OK = re.compile(r"^[A-Za-z0-9 #\-]+$")   # TE guide 22.11: letters, digits, '-', '#', space


# ------------------------------------------------------------------ integers
def sat(v):
    return I32_MIN if v < I32_MIN else I32_MAX if v > I32_MAX else v


def mul_q28(word, gain):
    """floor(word * gain / 2^28), saturated. gain is unsigned Q4.28 (< 16)."""
    return sat((word * gain) >> 28)


def db_to_q28(db):
    """A gain in dB as Q4.28, rounded to nearest; clamped below 16 (+24.08 dB)."""
    return min((16 << 28) - 1, max(0, int(math.floor(10 ** (db / 20) * Q28 + 0.5))))


def udiv(n, d):
    """Floor division of non-negative integers, as a shift-subtract loop does it."""
    assert n >= 0 and d > 0
    return n // d


class Dither:
    """Bounce phase 1's TPDF: two 16-bit LCG draws, difference, plus half an LSB."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state >> 16

    def sample(self, word):
        noise = self.next() - self.next()
        total = word + noise + 0x8000
        if total > I32_MAX or total < I32_MIN:
            total = I32_MIN if word < 0 else I32_MAX
        return total >> 16


# ------------------------------------------------------------------ files
class Sample:
    def __init__(self, frames, rate, channels):
        self.frames = frames          # list of tuples of Q31 words, one per channel
        self.rate = rate
        self.channels = channels
        self.touched = False          # set by any stage that changes values


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a WAV file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack_from("<I", data, pos + 4)[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif cid == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    tag, channels, rate, _, _, bits = fmt
    if tag not in (1, 0xFFFE) or bits not in (16, 24):
        raise SystemExit(f"{path}: 16- or 24-bit PCM only (got tag {tag}, {bits} bits)")
    width = bits // 8
    count = len(pcm) // (width * channels)
    frames = []
    for i in range(min(count, LOADER_FRAMES)):
        row = []
        for c in range(channels):
            o = (i * channels + c) * width
            if bits == 16:
                v = struct.unpack_from("<h", pcm, o)[0]
            else:
                # stock keeps 16-bit PCM in RAM: the top 16 bits, as truncation (U: stock may round)
                v = int.from_bytes(pcm[o:o + 3], "little", signed=True) >> 8
            row.append(v << 16)
        frames.append(tuple(row))
    return Sample(frames, rate, channels)


def write_wav(path, sample, seed, root=None):
    """16-bit PCM; dithered only when a stage changed the values. A 'smpl'
    chunk carries the root note when one is given (MIDI note number)."""
    dither = Dither(seed)
    out = bytearray()
    for row in sample.frames:
        for w in row:
            v = dither.sample(w) if sample.touched else w >> 16
            out += struct.pack("<h", v)
    ch, rate = sample.channels, sample.rate
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * 2, ch * 2, 16)
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if root is not None:
        period = int(round(1e9 / rate))
        smpl = struct.pack("<9I", 0, 0, period, root, 0, 0, 0, 0, 0)
        chunks += b"smpl" + struct.pack("<I", len(smpl)) + smpl
    chunks += b"data" + struct.pack("<I", len(out)) + bytes(out)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)


def next_name(path, kind="ed", exists=os.path.exists):
    """`kick.wav` -> `kick-ed1.wav` (or -s1 for chops), the first that's free,
    shortening the stem (never the folder) to stay within 95 characters."""
    folder, base = os.path.split(path)
    stem = os.path.splitext(base)[0]
    stem = "".join(ch for ch in stem if NAME_OK.match(ch)) or "sample"
    for n in range(1, 1000):
        suffix = f"-{kind}{n}.wav"
        room = PATH_MAX - len(folder) - 1 - len(suffix)
        if room < 1:
            raise SystemExit("folder path too long for a new name")
        candidate = os.path.join(folder, stem[:room].rstrip() + suffix)
        if not exists(candidate):
            return candidate
    raise SystemExit("no free name")


# ------------------------------------------------------------------ stages
def crop(s, begin, end):
    begin, end = max(0, begin), min(len(s.frames), end)
    if end <= begin:
        raise SystemExit("empty crop")
    s.frames = s.frames[begin:end]


def trim_bounds(s, threshold_db, pre_ms=5, post_ms=50):
    """Markers moved to the first and last frame above the threshold, with a
    short pre-roll and tail. Returns (begin, end); stock's markers show it."""
    level = int(math.floor(32767 * 10 ** (threshold_db / 20))) << 16
    loud = [i for i, row in enumerate(s.frames) if max(abs(w) for w in row) > level]
    if not loud:
        return 0, len(s.frames)
    pre, post = s.rate * pre_ms // 1000, s.rate * post_ms // 1000
    return max(0, loud[0] - pre), min(len(s.frames), loud[-1] + 1 + post)


def reverse(s):
    s.frames.reverse()


DC_K = 134930377045           # round(2 pi 10 2^31)


def dc_coefficient(rate):
    """The 10 Hz one-pole's feedback as Q31: 2^31 - floor((K + rate/2) / rate)."""
    return (1 << 31) - (DC_K + rate // 2) // rate


def mul31(a, b):
    """floor(a * b / 2^31), saturated: Blackfin's (T) multiply, q31::multiply."""
    return sat((a * b) >> 31)


def dc_remove(s):
    """One-pole high-pass at about 10 Hz: y = sat(sat(x - x1) + floor(y1 * a / 2^31))."""
    a = dc_coefficient(s.rate)
    x1 = [0] * s.channels
    y1 = [0] * s.channels
    out = []
    for row in s.frames:
        new = []
        for c, x in enumerate(row):
            y = sat(sat(x - x1[c]) + mul31(y1[c], a))
            x1[c], y1[c] = x, y
            new.append(y)
        out.append(tuple(new))
    s.frames = out
    s.touched = True


def peak(s):
    return max((abs(w) for row in s.frames for w in row), default=0)


def gain(s, g):
    if g == Q28:
        return
    s.frames = [tuple(mul_q28(w, g) for w in row) for row in s.frames]
    s.touched = True


def normalise_gain(s, target_db):
    """The Q4.28 gain that brings the peak to target_db: floor(T * 2^28 / peak)."""
    p = peak(s) >> 16
    if p == 0:
        return Q28
    target = int(math.floor(32767 * 10 ** (target_db / 20)))
    return min((16 << 28) - 1, udiv(target << 28, p))


def fade(s, in_ms, out_ms):
    """Linear fades: gain(n) = n * floor(2^28 / length), from 0 at the edge."""
    n = len(s.frames)
    fi = min(n, s.rate * in_ms // 1000) if in_ms else 0
    fo = min(n, s.rate * out_ms // 1000) if out_ms else 0
    if not fi and not fo:
        return
    step_in = udiv(Q28, fi) if fi else 0
    step_out = udiv(Q28, fo) if fo else 0
    out = []
    for i, row in enumerate(s.frames):
        g = Q28
        if i < fi:
            g = i * step_in
        if n - 1 - i < fo:
            g = min(g, (n - 1 - i) * step_out)
        out.append(row if g == Q28 else tuple(mul_q28(w, g) for w in row))
    s.frames = out
    s.touched = True


def channels(s, mode):
    """Output channels: 'keep', 'mono' (floor((L + R) / 2)), 'L' or 'R'."""
    if mode == 'keep' or s.channels < 2:
        return
    if mode == 'mono':
        s.frames = [((l + r) >> 1,) for l, r in s.frames]
        s.touched = True
    else:
        k = 0 if mode == 'L' else 1
        s.frames = [(row[k],) for row in s.frames]
    s.channels = 1


def bake(s, start=None, end=None, trim_db=None, rev=False, dc=False, gain_db=None,
         normalise_db=None, fade_in=0, fade_out=0, chans='keep'):
    """The trim page's chain, in its fixed order: markers, reverse, gain or
    normalise (on the 16-bit samples), DC, fades, channels. Returns the gain
    applied, as Q4.28. The device (sample_studio.cpp) runs the same order."""
    b, e = (start or 0), (end if end is not None else len(s.frames))
    if trim_db is not None:
        tb, te = trim_bounds(s, trim_db)
        b, e = max(b, tb), min(e, te)
    crop(s, b, e)
    if rev:
        reverse(s)
    g = Q28
    if normalise_db is not None:
        g = normalise_gain(s, normalise_db)
    elif gain_db is not None:
        g = db_to_q28(gain_db)
    gain(s, g)
    if dc:
        dc_remove(s)
    fade(s, fade_in, fade_out)
    channels(s, chans)
    return g


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?")
    ap.add_argument("output", nargs="?")
    ap.add_argument("--name", help="print the next free -edN name for this path and stop")
    ap.add_argument("--start", type=int)
    ap.add_argument("--end", type=int)
    ap.add_argument("--trim", type=float)
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--dc", action="store_true")
    ap.add_argument("--gain", type=float)
    ap.add_argument("--normalise", type=float)
    ap.add_argument("--fade-in", type=int, default=0)
    ap.add_argument("--fade-out", type=int, default=0)
    ap.add_argument("--chans", choices=('keep', 'mono', 'L', 'R'), default='keep')
    ap.add_argument("--root", type=int)
    ap.add_argument("--seed", type=lambda v: int(v, 0), default=0x9E3779B9)
    a = ap.parse_args(argv)
    if a.name:
        print(next_name(a.name))
        return 0
    if not (a.input and a.output):
        ap.error("input and output are required")
    s = read_wav(a.input)
    g = bake(s, a.start, a.end, a.trim, a.reverse, a.dc, a.gain, a.normalise, a.fade_in, a.fade_out, a.chans)
    write_wav(a.output, s, a.seed, a.root)
    print(f"{a.output}: {len(s.frames)} frames, {s.channels} ch, {s.rate} Hz, gain {g / Q28:.4f}, "
          f"{'dithered' if s.touched else 'copied bit-exact'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
