#!/usr/bin/env python3
"""Sample Studio's whole render chain, stage 3: an exact integer transcription of
the device's sample_studio_dsp.hpp and of sample_studio.cpp's writer.

    python3 studio_chain.py in.wav out.wav [--start F] [--end F] [--reverse]
        [--gain DB | --normalise DB] [--dc] [--fade-in MS] [--fade-out MS] [--chans keep|mono|L|R]
        [--stretch PERCENT] [--pitch ST] [--mode tape|beat] [--grain MS]
        [--rate-index N] [--bits-index N] [--drive N] [--sweep N] [--comp N]
        [--slices "0,10000,20000,..."  --chop-fade MS]

The values come from the device's "saved ..." log line. A save made on the
OP-XY should null against this, sample for sample (16-bit originals): the chain
is the same 32-bit integer arithmetic, in the same order:
    source (markers, reverse, gain) -> beat (WSOLA) -> resampler -> color -> DC
    -> fades -> channels -> dither
With --slices, writes out-s1.wav, out-s2.wav, ... with the chop fade at both
ends of each slice.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import studio_ref as R  # noqa: E402

Q28 = 1 << 28
M32 = 0xFFFFFFFF
I32_MIN, I32_MAX = -(1 << 31), (1 << 31) - 1
SEMITONE_Q16 = [round(65536 * 2 ** (st / 12)) for st in range(-24, 25)]
CUTOFF_HZ = [round(50 * (12000 / 50) ** (i / 47)) for i in range(48)]
CUTOFF_Q31 = [int(math.floor(2 * math.sin(math.pi * h / 88200) * 2 ** 31 + 0.5)) for h in CUTOFF_HZ]
RATE_HZ = [0, 32000, 24000, 16000, 12000, 8000, 6000, 4000, 2000]
BITS = [16, 12, 10, 8, 6, 5, 4, 3, 2]
LEVEL_GAIN = [R.db_to_q28(d) for d in range(-30, 21)]
NORMAL_TARGET = [int(math.floor(32767 * 10 ** (t / 20))) for t in (0, -1, -3, -6)]
# sample-studio-017: the filter sweep, bits on a level-following step, comp (sample_studio_dsp.hpp).
SWEEP_HALF = 0x2CCCCCCD
SWEEP_LIMIT = 1 << 30
BITS_RELEASE, BITS_FLOOR = 12, 1 << 16
COMP_THRESHOLD = [int(math.floor(0x7FFFFFFF * 10 ** (-0.3 * a / 20))) for a in range(101)]
COMP_INVERSE = [(65536 * 100) // (100 + 5 * a) for a in range(101)]


def _comp_out_fs(a):
    t = COMP_THRESHOLD[a]
    d = 0x7FFFFFFF - t
    return t + ((d >> 16) * COMP_INVERSE[a] + (((d & 0xFFFF) * COMP_INVERSE[a]) >> 16))


COMP_MAKEUP = [min(0x7FFFFFFF, ((1 << 28) * (1 << 31)) // _comp_out_fs(a)) for a in range(101)]
COMP_ATTACK, COMP_RELEASE, COMP_EVERY = 3, 13, 16


def sweep_clamp(v):
    return -SWEEP_LIMIT if v < -SWEEP_LIMIT else SWEEP_LIMIT if v > SWEEP_LIMIT else v


def frac20(r, dn):
    q = 0
    for _ in range(20):
        r = (r << 1) & M32
        bit = 1 if r >= dn else 0
        r = (r - (dn & ((0 - bit) & M32))) & M32
        q = ((q << 1) | bit) & M32
    return q


def mul_q28_sat(x, gain):
    r = mul31(x, wrap(gain))
    return I32_MAX if r > (I32_MAX >> 3) else I32_MIN if r < -(1 << 28) else r * 8


def wrap(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def sat(v):
    return I32_MIN if v < I32_MIN else I32_MAX if v > I32_MAX else v


def mul31(a, b):
    return sat((a * b) >> 31)


# Drive (sample-studio-006), sample_studio_dsp.hpp drive_word: gain 1 + 5 drive/100,
# the tanh-shaped t(v) = v (27 + v^2) / (27 + 9 v^2) held at +-1 from |v| = 3, then
# makeup 1 - 0.3 drive/100. All 32-bit, the division to 20 bits.
DRIVE_GAIN_STEP, DRIVE_MAKEUP_STEP, DRIVE_BITS = 13421772, 6442450, 20


DRIVE_RECIP_BASE = 27 << 22
DRIVE_RECIP = [(1 << 57) // (DRIVE_RECIP_BASE + i * (1 << 20)) for i in range(326)]


def drive_word(x, drive):
    v = mul31(x, Q28 + DRIVE_GAIN_STEP * drive)
    three = 3 << 28
    if v >= three:
        t = I32_MAX
    elif v <= -three:
        t = -I32_MAX
    else:
        v2 = mul31(v, v)
        n = mul31(v, (27 << 25) + v2)
        dn = (27 << 22) + 9 * (v2 >> 3)
        r = abs(n)
        if r >= dn:
            r = dn - 1
        # sample-studio-018: 1/dn from the reciprocal table, interpolated (sample_studio_dsp.hpp)
        at = dn - DRIVE_RECIP_BASE
        index = at >> 20
        high, low = DRIVE_RECIP[index], DRIVE_RECIP[index + 1]
        recip = high - mul31(high - low, (at & 0xFFFFF) << 11)
        p = mul31(wrap(dn << 2), recip)
        recip = wrap(mul31(recip, wrap(((1 << 29) - p) & M32)) << 3)
        q = (mul31(wrap(r << 2), recip) & M32) >> 8
        rem = wrap(((r << 20) - q * dn) & M32)
        if rem < 0:
            q -= 1
            rem += dn
        elif rem >= dn:
            q += 1
            rem -= dn
        if rem < 0:
            q -= 1
        elif rem >= dn:
            q += 1
        m = q << (31 - DRIVE_BITS)
        t = -m if n < 0 else m
    return mul31(t, I32_MAX - DRIVE_MAKEUP_STEP * drive)


def lerp(a, b, frac):
    d = (b >> 1) - (a >> 1)
    return sat(a + mul31(d, frac) * 2)


def gain_word(x, gain):
    high = wrap(x * (gain >> 16))
    low = wrap(x * (gain & 0xFFFF))
    if high > 0x7FFFFFF:
        return I32_MAX
    if high < -0x8000000:
        return I32_MIN
    return sat(wrap(high * 16) + (low >> 12))


def normalise_gain(peak, target):
    if not peak:
        return Q28
    if peak * 16 <= target:
        return 0xFFFFFFFF
    return (target << 28) // peak


class Settings:
    def __init__(self, **kw):
        self.begin = 0
        self.end = 0
        self.reverse = 0
        self.gain = Q28
        self.dc = 0
        self.stretch = 100
        self.pitch = 0
        self.beat = 1
        self.grain_ms = 40
        self.rate_index = self.bits_index = self.drive = 0
        self.sweep = 0          # -48..-1 low-pass, 0 off, 1..48 high-pass
        self.comp = 0
        self.__dict__.update(kw)


def time_active(s):
    return s.stretch != 100 or s.pitch != 0


def color_active(s):
    return bool(s.rate_index or s.bits_index or s.drive or s.sweep or s.comp)


MAX_GRAIN_MS = 160


def grain_ms_for(s):
    """sample_studio_dsp.hpp grain_ms_for (sample-studio-009): the grain grown with compression."""
    page = max(20, min(120, s.grain_ms))
    ratio = SEMITONE_Q16[max(-24, min(24, s.pitch)) + 24]
    percent = (s.stretch * ratio) >> 16
    if percent >= 100:
        return page
    grown = page * 100 // percent if percent else MAX_GRAIN_MS
    return (page if page > MAX_GRAIN_MS else MAX_GRAIN_MS) if grown > MAX_GRAIN_MS else grown


class Pipeline:
    """sample_studio_dsp.hpp's Pipeline, line for line."""

    def __init__(self, data, channels, rate, s):
        self.data, self.channels, self.rate, self.s = data, channels, rate, s
        self.length = s.end - s.begin if s.end > s.begin else 0
        self.read = 0
        ratio = SEMITONE_Q16[max(-24, min(24, s.pitch)) + 24]
        self.beat_on = bool(s.beat) and time_active(s)
        if self.beat_on:
            g = rate * grain_ms_for(s) // 1000
            g &= ~1
            self.grain = max(g, 64)
            self.hop = self.grain // 2
            self.step_q31 = 0x7FFFFFFF // self.hop
            denom = (s.stretch * ratio) >> 8
            per_out = (6553600 << 8) // denom if denom else 65536
            low = self.hop * (per_out & 0xFFFF)
            self.advance_int = self.hop * (per_out >> 16) + (low >> 16)
            self.advance_frac = low & 0xFFFF
            self.next_int = self.next_frac = 0
            self.previous = self.current = 0
            self.index = 0
            self.first = True
            self.beat_done = False
            self.speed_q16 = ratio
        else:
            self.speed_q16 = 65536 if s.stretch == 100 else 6553600 // s.stretch
        self.pos_int = self.pos_frac = 0
        self.window = [[0, 0], [0, 0]]
        self.loaded = 0
        self.upstream_done = False
        self.rate_acc = 0
        self.held_valid = False
        self.held = [0, 0]
        self.low = [0, 0]
        self.band = [0, 0]
        self.bits_env = 0
        self.comp_env = 0
        self.comp_gain = 1 << 20
        self.comp_count = 0
        self.dc_a = R.dc_coefficient(rate) if s.dc else 0
        self.x1 = [0, 0]
        self.y1 = [0, 0]

    def at(self, i, c):
        s = self.s
        f = s.end - 1 - i if s.reverse else s.begin + i
        x = self.data[f][c]
        return x * 65536 if s.gain == Q28 else gain_word(x, s.gain)

    def probe(self, i):
        # both channels of a stereo source (sample-studio-008), 12 bits
        if self.channels > 1:
            return (self.at(i, 0) >> 21) + (self.at(i, 1) >> 21)
        return self.at(i, 0) >> 20

    def beat_frame(self):
        if self.beat_done:
            return None
        if self.index == 0:
            if self.first:
                self.current = 0
            else:
                total = self.next_frac + self.advance_frac
                self.next_int = (self.next_int + self.advance_int + (total >> 16)) & M32
                self.next_frac = total & 0xFFFF
                self.previous = self.current
                natural = self.previous + self.hop
                nominal = self.next_int
                if nominal + self.hop > self.length or natural + self.hop > self.length:
                    self.beat_done = True
                    return None
                span = min(self.hop // 2, 256)
                best, best_score = nominal, -0x7FFFFFFF
                for d in range(-span, span + 1, 8):
                    c = nominal + d
                    if c < 0 or c + self.hop > self.length:
                        continue
                    score = 0
                    for j in range(0, self.hop, 16):
                        score += self.probe(c + j) * self.probe(natural + j)
                    if score > best_score:
                        best_score, best = score, c
                self.current = best
        out = [0, 0]
        for c in range(self.channels):
            if self.first:
                out[c] = self.at(self.index, c)
            else:
                fade_in = self.index * self.step_q31
                fade_out = (self.hop - self.index) * self.step_q31
                out[c] = sat(mul31(self.at(self.previous + self.hop + self.index, c), fade_out) +
                             mul31(self.at(self.current + self.index, c), fade_in))
        self.index += 1
        if self.index == self.hop:
            self.index = 0
            self.first = False
        return out

    def upstream(self):
        if self.beat_on:
            return self.beat_frame()
        if self.read >= self.length:
            return None
        out = [self.at(self.read, c) for c in range(self.channels)] + [0] * (2 - self.channels)
        self.read += 1
        return out

    def timed(self):
        if self.speed_q16 == 65536:
            return self.upstream()
        while self.loaded < self.pos_int + 2:
            f = None if self.upstream_done else self.upstream()
            if f is None:
                self.upstream_done = True
                break
            slot = self.loaded & 1
            for c in range(self.channels):
                self.window[slot][c] = f[c]
            self.loaded += 1
        if self.pos_int + 2 > self.loaded:
            return None
        a, b = self.pos_int & 1, (self.pos_int + 1) & 1
        out = [0, 0]
        for c in range(self.channels):
            out[c] = lerp(self.window[a][c], self.window[b][c], self.pos_frac << 15)
        self.pos_frac += self.speed_q16 & 0xFFFF
        self.pos_int += (self.speed_q16 >> 16) + (self.pos_frac >> 16)
        self.pos_frac &= 0xFFFF
        return out

    def color(self, w):
        s = self.s
        ch = 1 if self.channels < 2 else 2
        if s.drive:
            for c in range(ch):
                w[c] = drive_word(w[c], s.drive)
        if s.bits_index:
            m = max(abs(w[c]) for c in range(ch))
            if m > self.bits_env:
                self.bits_env = m
            else:
                self.bits_env -= self.bits_env >> BITS_RELEASE
            top = (self.bits_env | BITS_FLOOR).bit_length() - 1
            b = BITS[s.bits_index]
            if top + 1 > b:
                shift = top + 1 - b
                half = 1 << (shift - 1)
                mask = ~((1 << shift) - 1) & M32
                for c in range(ch):
                    w[c] = wrap((sat(w[c] + half) & M32) & mask)
        if s.rate_index and RATE_HZ[s.rate_index] < self.rate:
            self.rate_acc = (self.rate_acc + RATE_HZ[s.rate_index]) & M32
            if not self.held_valid or self.rate_acc >= self.rate:
                if self.held_valid:
                    self.rate_acc -= self.rate
                self.held = [w[0], w[1]]
                self.held_valid = True
            w[0], w[1] = self.held
        if s.sweep:
            index = min(abs(s.sweep), len(CUTOFF_Q31))
            f = CUTOFF_Q31[index - 1]
            for c in range(ch):
                x = w[c] >> 2
                high = 0
                for _ in range(2):
                    self.low[c] = sweep_clamp(sat(self.low[c] + mul31(f, self.band[c])))
                    damp = mul31(SWEEP_HALF, self.band[c])
                    high = sweep_clamp(sat(sat(x - self.low[c]) - sat(damp + damp)))
                    self.band[c] = sweep_clamp(sat(self.band[c] + mul31(f, high)))
                y = self.low[c] if s.sweep < 0 else high
                w[c] = I32_MAX if y >= (1 << 29) else I32_MIN if y < -(1 << 29) else y * 4
        if s.comp:
            a = min(100, s.comp)
            m = max(abs(w[c]) for c in range(ch))
            if m > self.comp_env:
                self.comp_env += (m - self.comp_env) >> COMP_ATTACK
            else:
                self.comp_env -= self.comp_env >> COMP_RELEASE
            if self.comp_count % COMP_EVERY == 0:
                t, env = COMP_THRESHOLD[a], self.comp_env
                if env <= t:
                    self.comp_gain = 1 << 20
                else:
                    over, inv = env - t, COMP_INVERSE[a]
                    num = (t + (over >> 16) * inv + (((over & 0xFFFF) * inv) >> 16)) & M32
                    self.comp_gain = frac20(num, env) if num < env else 1 << 20
            self.comp_count = (self.comp_count + 1) & M32
            for c in range(ch):
                x = w[c]
                if self.comp_gain < (1 << 20):
                    x = mul31(x, self.comp_gain << 11)
                w[c] = mul_q28_sat(x, COMP_MAKEUP[a])
        if ch < 2:
            w[1] = w[0]

    def fill(self, n):
        """sample_studio_dsp.hpp fill(): up to n frames, the colour stage a step across the block at a
        time (sample-studio-018), as two lists (left, right)."""
        l, r = [], []
        for _ in range(n):
            w = self.timed()
            if w is None:
                break
            w = list(w) + [0] * (2 - len(w))
            l.append(w[0])
            r.append(w[0] if self.channels < 2 else w[1])
        k = len(l)
        s = self.s
        if color_active(s):
            ch = 1 if self.channels < 2 else 2
            w = [l, r]
            if s.drive:
                for c in range(ch):
                    for j in range(k):
                        w[c][j] = drive_word(w[c][j], s.drive)
            if s.bits_index:
                b = BITS[s.bits_index]
                for j in range(k):
                    m = max(abs(w[c][j]) for c in range(ch))
                    if m > self.bits_env:
                        self.bits_env = m
                    else:
                        self.bits_env -= self.bits_env >> BITS_RELEASE
                    top = (self.bits_env | BITS_FLOOR).bit_length() - 1
                    if top + 1 > b:
                        shift = top + 1 - b
                        half = 1 << (shift - 1)
                        mask = ~((1 << shift) - 1) & M32
                        for c in range(ch):
                            w[c][j] = wrap((sat(w[c][j] + half) & M32) & mask)
            if s.rate_index and RATE_HZ[s.rate_index] < self.rate:
                for j in range(k):
                    self.rate_acc = (self.rate_acc + RATE_HZ[s.rate_index]) & M32
                    if not self.held_valid or self.rate_acc >= self.rate:
                        if self.held_valid:
                            self.rate_acc -= self.rate
                        self.held = [l[j], r[j]]
                        self.held_valid = True
                    l[j], r[j] = self.held
            if s.sweep:
                index = min(abs(s.sweep), len(CUTOFF_Q31))
                f = CUTOFF_Q31[index - 1]
                for c in range(ch):
                    lo, bd = self.low[c], self.band[c]
                    for j in range(k):
                        x = w[c][j] >> 2
                        high = 0
                        for _ in range(2):
                            lo = sweep_clamp(sat(lo + mul31(f, bd)))
                            damp = mul31(SWEEP_HALF, bd)
                            high = sweep_clamp(sat(sat(x - lo) - sat(damp + damp)))
                            bd = sweep_clamp(sat(bd + mul31(f, high)))
                        y = lo if s.sweep < 0 else high
                        w[c][j] = I32_MAX if y >= (1 << 29) else I32_MIN if y < -(1 << 29) else y * 4
                    self.low[c], self.band[c] = lo, bd
            if s.comp:
                a = min(100, s.comp)
                for j in range(k):
                    m = max(abs(w[c][j]) for c in range(ch))
                    if m > self.comp_env:
                        self.comp_env += (m - self.comp_env) >> COMP_ATTACK
                    else:
                        self.comp_env -= self.comp_env >> COMP_RELEASE
                    if self.comp_count % COMP_EVERY == 0:
                        t, env = COMP_THRESHOLD[a], self.comp_env
                        if env <= t:
                            self.comp_gain = 1 << 20
                        else:
                            over, inv = env - t, COMP_INVERSE[a]
                            num = (t + (over >> 16) * inv + (((over & 0xFFFF) * inv) >> 16)) & M32
                            self.comp_gain = frac20(num, env) if num < env else 1 << 20
                    self.comp_count = (self.comp_count + 1) & M32
                    for c in range(ch):
                        x = w[c][j]
                        if self.comp_gain < (1 << 20):
                            x = mul31(x, self.comp_gain << 11)
                        w[c][j] = mul_q28_sat(x, COMP_MAKEUP[a])
            if ch < 2:
                for j in range(k):
                    r[j] = l[j]
        if s.dc:
            for c in range(min(self.channels, 2)):
                x = l if c == 0 else r
                for j in range(k):
                    y = sat(sat(x[j] - self.x1[c]) + mul31(self.y1[c], self.dc_a))
                    self.x1[c], self.y1[c] = x[j], y
                    x[j] = y
        return l, r

    def next(self):
        out = self.timed()
        if out is None:
            return None
        out = list(out) + [0] * (2 - len(out))
        if self.channels < 2:
            out[1] = out[0]
        if color_active(self.s):
            self.color(out)
        if self.s.dc:
            for c in range(self.channels):
                y = sat(sat(out[c] - self.x1[c]) + mul31(self.y1[c], self.dc_a))
                self.x1[c], self.y1[c] = out[c], y
                out[c] = y
        return out


MAX_FRAMES = 882000


def render(sample, s, level=0, normal=0, fade_in=0, fade_out=0, chans='keep', slices=None, chop_fade=0,
           seed=0x9E3779B9):
    """sample_studio.cpp's render: the 16-bit frames out, as lists of tuples
    (one list, or one per slice)."""
    data = [tuple(w >> 16 for w in row) for row in sample.frames]
    channels, rate = sample.channels, sample.rate
    if normal:
        peak = max((abs(v) for row in data[s.begin:s.end] for v in row), default=0)
        s.gain = normalise_gain(peak, NORMAL_TARGET[normal - 1])
    else:
        s.gain = LEVEL_GAIN[level + 30]
    if channels < 2:
        chans = 'keep'
    out_channels = channels if chans == 'keep' else 1
    p = Pipeline(data, channels, rate, s)
    if time_active(s):
        n = 0
        while n < MAX_FRAMES and p.next() is not None:
            n += 1
        p = Pipeline(data, channels, rate, s)
    else:
        n = s.end - s.begin
    n = min(n, MAX_FRAMES)
    fi = min(n, rate * fade_in // 1000) if fade_in else 0
    fo = min(n, rate * fade_out // 1000) if fade_out else 0
    touched = (s.gain != Q28 or s.dc or fi or fo or chans == 'mono' or time_active(s) or color_active(s) or
               (slices is not None and chop_fade))
    dither = R.Dither(seed)

    def put(i, length, w, fi, fo):
        g = Q28
        if i < fi:
            g = i * (Q28 // fi)
        back = length - 1 - i
        if back < fo and back * (Q28 // fo) < g:
            g = back * (Q28 // fo)
        v = list(w)
        if g < Q28:
            for c in range(channels):
                v[c] = mul31(v[c], g << 3)
        o = [v[0], v[1]]
        if chans == 'mono':
            o[0] = (v[0] >> 1) + (v[1] >> 1) + (v[0] & v[1] & 1)
        elif chans == 'R':
            o[0] = v[1]
        return tuple(dither.sample(o[c]) if touched else o[c] >> 16 for c in range(out_channels))

    frames = []
    for i in range(n):
        w = p.next() or [0, 0]
        frames.append(w)
    if slices is None:
        return [put(i, n, frames[i], fi, fo) for i in range(n)], out_channels
    cuts = list(slices) + [n]
    out = []
    for k in range(len(cuts) - 1):
        a, b = cuts[k], cuts[k + 1]
        length = b - a
        fade = min(length, rate * chop_fade // 1000)
        out.append([put(i - a, length, frames[i], fade, fade) for i in range(a, b)])
    return out, out_channels


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--start", type=int)
    ap.add_argument("--end", type=int)
    ap.add_argument("--reverse", action="store_true")
    ap.add_argument("--gain", type=int, default=0, help="level in whole dB, -30..20")
    ap.add_argument("--normalise", type=int, default=0, help="1..4: 0, -1, -3, -6 dBFS")
    ap.add_argument("--dc", action="store_true")
    ap.add_argument("--fade-in", type=int, default=0)
    ap.add_argument("--fade-out", type=int, default=0)
    ap.add_argument("--chans", choices=('keep', 'mono', 'L', 'R', '0', '1', '2', '3'), default='keep',
                    help="as named, or the device log's number: 0 keep, 1 mono, 2 L, 3 R")
    ap.add_argument("--stretch", type=int, default=100)
    ap.add_argument("--pitch", type=int, default=0)
    ap.add_argument("--mode", choices=('tape', 'beat'), default='beat')
    ap.add_argument("--grain", type=int, default=40)
    for name in ("rate-index", "bits-index", "drive", "sweep", "comp"):
        ap.add_argument("--" + name, type=int, default=0)
    ap.add_argument("--slices", help="comma-separated output frames where each slice starts")
    ap.add_argument("--chop-fade", type=int, default=0)
    a = ap.parse_args(argv)
    if a.chans.isdigit():
        a.chans = ('keep', 'mono', 'L', 'R')[int(a.chans)]
    sample = R.read_wav(a.input)
    s = Settings(begin=a.start or 0, end=a.end if a.end is not None else len(sample.frames), reverse=int(a.reverse),
                 dc=int(a.dc), stretch=a.stretch, pitch=a.pitch, beat=int(a.mode == 'beat'), grain_ms=a.grain,
                 rate_index=a.rate_index, bits_index=a.bits_index, drive=a.drive, sweep=a.sweep, comp=a.comp)
    slices = [int(v) for v in a.slices.split(',')] if a.slices else None
    out, out_channels = render(sample, s, a.gain, a.normalise, a.fade_in, a.fade_out, a.chans, slices, a.chop_fade)
    outputs = [out] if slices is None else out
    for k, frames in enumerate(outputs):
        path = a.output if slices is None else a.output.replace('.wav', f'-s{k + 1}.wav')
        result = R.Sample([tuple(v << 16 for v in row) for row in frames], sample.rate, out_channels)
        R.write_wav(path, result, 0)   # 16-bit values already final: write without touching them
        print(f"{path}: {len(frames)} frames, {out_channels} ch")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
