// Sample Studio bakes:
// the parts that need no stock call. Its arithmetic is the reference's
// (tests/studio_ref.py): Q4.28 gains with floor
// semantics, bounce phase 1's TPDF dither, and a copy, not a dither, when the
// gain is unity. On the device, editor.cpp and player.cpp use it.
#pragma once
#include <opxy/q31.hpp>
#include <opxy/types.hpp>

namespace opxy { namespace sample_bake {

const u32 kUnity = 1u << 28;            // Q4.28 1.0
const u32 kGainCap = 0xffffffffu;       // just under 16.0, the reference's (16 << 28) - 1
const u32 kTarget = 29203;              // floor(32767 * 10^(-1/20)): -1 dBFS, studio_ref normalise_gain
const u32 kHeaderBytes = 44;

// floor(target * 2^28 / peak), capped at kGainCap; unity for silence. A
// shift-subtract division over the 43-bit numerator, so no library divide.
inline u32 normalise_gain(u32 peak, u32 target) {
    if (!peak) return kUnity;
    if (peak * 16u <= target) return kGainCap;  // the quotient would reach 2^32
    u32 q = 0, r = 0;
    for (i32 bit = 42; bit >= 0; --bit) {
        const u32 next = bit >= 28 ? (target >> (bit - 28)) & 1u : 0u;
        r = (r << 1) | next;
        q <<= 1;
        if (r >= peak) {
            r -= peak;
            q |= 1u;
        }
    }
    return q;
}

// The reference's mul_q28 on a 16-bit sample shifted to Q31: floor(x * gain / 2^12).
// gain splits into 16-bit halves so each product fits 32 bits. Callers keep
// |x| * gain under 2^43 (a normalising gain does: |x| <= peak), so no saturation.
inline i32 scale(i32 x, u32 gain) {
    const i32 high = x * static_cast<i32>(gain >> 16);
    const i32 low = x * static_cast<i32>(gain & 0xffffu);
    return high * 16 + (low >> 12);
}

// TPDF dither to 16 bits.
struct Dither {
    u32 state;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 16;
    }
    i32 sample(i32 word) {
        const i32 noise = static_cast<i32>(next()) - static_cast<i32>(next());
        i32 sum;
        if (__builtin_add_overflow(word, noise + 0x8000, &sum)) sum = word < 0 ? static_cast<i32>(0x80000000u) : 0x7fffffff;
        return sum >> 16;
    }
};
const u32 kSeed = 0x9e3779b9u;          // studio_ref's default seed

// ---- Sample Studio's trim page chain (studio_ref.bake), in its fixed order:
// markers and reverse, then gain on the 16-bit sample, DC, fades, channels.
// Every product stays in 32 bits, so each stage matches the reference exactly.

// The level control, -30..+20 dB in whole dB, as studio_ref.db_to_q28 rounds it.
const u32 kLevelGain[51] = {
    8488674u, 9524449u, 10686608u, 11990571u, 13453642u, 15095235u, 16937132u, 19003775u, 21322586u,
    23924335u, 26843546u, 30118954u, 33794022u, 37917516u, 42544153u, 47735324u, 53559915u, 60095213u,
    67427938u, 75655391u, 84886745u, 95244494u, 106866080u, 119905714u, 134536424u, 150952350u,
    169371322u, 190037749u, 213225862u, 239243352u, 268435456u, 301189535u, 337940217u, 379175160u,
    425441527u, 477353244u, 535599149u, 600952130u, 674279380u, 756553907u, 848867446u, 952444939u,
    1068660799u, 1199057137u, 1345364236u, 1509523501u, 1693713225u, 1900377495u, 2132258619u,
    2392433520u, 2684354560u};
const i32 kLevelMin = -30, kLevelMax = 20;
inline u32 level_gain(i32 db) { return kLevelGain[(db < kLevelMin ? kLevelMin : db > kLevelMax ? kLevelMax : db) - kLevelMin]; }
// Normalise targets for 0, -1, -3 and -6 dBFS: floor(32767 * 10^(dB/20)).
const u32 kNormalTarget[4] = {32767u, 29203u, 23197u, 16422u};
const i32 kNormalDb[4] = {0, -1, -3, -6};
// Silence thresholds for -60..-24 dB in 6 dB steps: floor(32767 * 10^(dB/20)).
const u32 kSilenceThreshold[7] = {32u, 65u, 130u, 260u, 519u, 1036u, 2067u};
const i32 kSilenceDb[7] = {-60, -54, -48, -42, -36, -30, -24};

inline i32 saturate_add(i32 a, i32 b) {
    i32 sum;
    if (__builtin_add_overflow(a, b, &sum)) return a < 0 ? static_cast<i32>(0x80000000u) : 0x7fffffff;
    return sum;
}
// sat(floor(x * gain / 2^12)) for a 16-bit sample x: studio_ref.mul_q28 on x << 16.
inline i32 gain_word(i32 x, u32 gain) {
    const i32 high = x * static_cast<i32>(gain >> 16);   // |x| <= 2^15, gain >> 16 < 2^16
    const i32 low = x * static_cast<i32>(gain & 0xffffu);
    if (high > 0x7ffffff) return 0x7fffffff;
    if (high < -0x8000000) return static_cast<i32>(0x80000000u);
    return saturate_add(high * 16, low >> 12);
}
// floor(a * b / 2^31): Blackfin's (T) multiply, the SDK's q31::multiply.
inline i32 mul31(i32 a, i32 b) { return opxy::q31::multiply(a, b); }
// One-pole high-pass at 10 Hz: y = sat(sat(x - x1) + floor(y1 * a / 2^31)),
// a = 2^31 - floor((K + rate/2) / rate), K = round(2 pi 10 2^31).
struct DcBlock {
    i32 x1[2], y1[2], a;
    void start(u32 rate) {
        // K = 134930377045 = 31 * 2^32 + 1786390869, divided by shift and subtract.
        const u32 hi = 31u, lo = 1786390869u;
        const u32 add = rate >> 1;
        const u32 low = lo + add;
        const u32 high = hi + (low < lo ? 1u : 0u);
        u32 q = 0, r = 0;
        for (i32 bit = 63; bit >= 0; --bit) {
            const u32 next = bit >= 32 ? (high >> (bit - 32)) & 1u : (low >> bit) & 1u;
            r = (r << 1) | next;
            if (bit < 32) q <<= 1;
            if (r >= rate) {
                r -= rate;
                if (bit < 32) q |= 1u;
            }
        }
        a = static_cast<i32>(0x80000000u - q);
        x1[0] = x1[1] = y1[0] = y1[1] = 0;
    }
    i32 process(u32 c, i32 x) {
        const i32 y = saturate_add(saturate_add(x, -x1[c]), mul31(y1[c], a));
        x1[c] = x;
        y1[c] = y;
        return y;
    }
};
// Fades: gain(n) = n * floor(2^28 / length) from 0 at the edge, applied as
// floor(w * gain / 2^28) = mul31(w, gain << 3) (gain < 2^28).
inline i32 fade_word(i32 w, u32 gain) { return mul31(w, static_cast<i32>(gain << 3)); }
// Output channels: as they are, a mono mix floor((L + R) / 2), L or R.
enum Channels : u32 { kKeep = 0, kMono = 1, kLeft = 2, kRight = 3 };
inline i32 mono_word(i32 l, i32 r) { return (l >> 1) + (r >> 1) + (l & r & 1); }

inline void put16(unsigned char* p, u32 v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
inline void put32(unsigned char* p, u32 v) { put16(p, v & 0xffff); put16(p + 2, v >> 16); }
inline u32 get32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
inline bool same4(const unsigned char* p, const char* tag) {
    return p[0] == static_cast<unsigned char>(tag[0]) && p[1] == static_cast<unsigned char>(tag[1]) &&
           p[2] == static_cast<unsigned char>(tag[2]) && p[3] == static_cast<unsigned char>(tag[3]);
}
// A file's own root (editor.cpp A49): the first well-formed 'smpl' chunk's MIDI unity note (0..127,
// 60 = C4) and pitch fraction (a fraction of a semitone above it), as cents from note 0. Chunks are
// walked in order, each skipped by its size and its pad byte when odd, reading through them (the
// card has no seek here), at most `limit` bytes in all. False for no RIFF/WAVE, no such chunk, a short
// or out-of-range one only, or the limit reached. `read(to, n)` returns the bytes it read.
template <class Read>
bool wav_root_cents(Read& read, i32& cents, u32 limit, unsigned char* scratch, u32 scratch_size) {
    unsigned char h[12];
    if (scratch_size < 20 || read(h, 12) != 12 || !same4(h, "RIFF") || !same4(h + 8, "WAVE")) return false;
    u32 used = 12;
    unsigned char c[8];
    while (used + 8 <= limit && read(c, 8) == 8) {
        used += 8;
        const u32 size = get32(c + 4);
        u32 left = size + (size & 1u);
        if (left < size) return false;   // a size that wraps
        if (same4(c, "smpl") && size >= 20) {
            if (read(scratch, 20) != 20) return false;
            used += 20;
            left -= 20;
            const u32 unity = get32(scratch + 12), fraction = get32(scratch + 16);
            if (unity <= 127) {
                cents = static_cast<i32>(unity * 100u + (((fraction >> 16) * 100u) >> 16));
                return true;
            }
        }
        while (left) {
            if (used >= limit) return false;
            const u32 n = left < scratch_size ? left : scratch_size;
            if (read(scratch, n) != n) return false;
            left -= n;
            used += n;
        }
    }
    return false;
}

// A 44-byte header for 16-bit PCM at the sample's own rate and channels.
inline void wav_header(unsigned char* h, u32 frames, u32 channels, u32 rate) {
    const u32 block = 2 * channels;
    const u32 data = frames * block;
    const char* tags[] = {"RIFF", "WAVE", "fmt ", "data"};
    const u32 at[] = {0, 8, 12, 36};
    for (u32 t = 0; t < 4; ++t)
        for (u32 i = 0; i < 4; ++i) h[at[t] + i] = static_cast<unsigned char>(tags[t][i]);
    put32(h + 4, 36 + data);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, channels);
    put32(h + 24, rate);
    put32(h + 28, rate * block);
    put16(h + 32, block);
    put16(h + 34, 16);
    put32(h + 40, data);
}

// Bounded string building: false when it would not fit (size counts the terminator).
inline bool append(char* out, u32 size, u32& n, const char* text) {
    for (const char* p = text; *p; ++p) {
        if (n + 1 >= size) return false;
        out[n++] = *p;
    }
    out[n] = 0;
    return true;
}
// Decimal by repeated subtraction: no divide helper on the Blackfin. A table at
// namespace scope, not a function-local static (docs/toolchain.md).
const u32 kPowers[] = {1000000000u, 100000000u, 10000000u, 1000000u, 100000u, 10000u, 1000u, 100u, 10u, 1u};
inline bool append_number(char* out, u32 size, u32& n, u32 value) {
    const u32* powers = kPowers;
    bool started = false;
    for (u32 i = 0; i < 10; ++i) {
        char digit = '0';
        while (value >= powers[i]) {
            value -= powers[i];
            ++digit;
        }
        if (digit != '0' || started || i == 9) {
            started = true;
            const char one[2] = {digit, 0};
            if (!append(out, size, n, one)) return false;
        }
    }
    return true;
}
inline bool name_character(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '#' || c == '-';
}
// The stem of a path: after its last '/', before its last '.', keeping only the
// characters the OP-XY's names allow (studio_ref next_name).
inline void path_stem(const char* path, char* stem, u32 size) {
    const char* base = path;
    const char* dot = nullptr;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') { base = p + 1; dot = nullptr; }
        else if (*p == '.') dot = p;
    }
    u32 n = 0;
    for (const char* p = base; *p && p != dot && n + 1 < size; ++p)
        if (name_character(*p)) stem[n++] = *p;
    if (!n) {
        const char fallback[] = "sample";
        for (u32 i = 0; fallback[i] && n + 1 < size; ++i) stem[n++] = fallback[i];
    }
    stem[n] = 0;
}
// "<folder>/<stem>-ed<number><extension>", within 95 characters (stock's region
// paths): the stem is shortened, never the folder. False when it can't fit.
inline bool edit_path(char* out, u32 size, const char* folder, const char* stem, u32 number, const char* extension) {
    const u32 kPathMax = 95;
    char suffix[16];
    u32 s = 0;
    if (!append(suffix, sizeof suffix, s, "-ed") || !append_number(suffix, sizeof suffix, s, number)) return false;
    u32 folder_length = 0, extension_length = 0;
    while (folder[folder_length]) ++folder_length;
    while (extension[extension_length]) ++extension_length;
    if (folder_length + 1 + s + extension_length + 1 > kPathMax) return false;
    const u32 room = kPathMax - folder_length - 1 - s - extension_length;
    u32 n = 0;
    if (!append(out, size, n, folder) || !append(out, size, n, "/")) return false;
    u32 kept = 0;
    while (stem[kept] && kept < room) ++kept;
    while (kept && stem[kept - 1] == ' ') --kept;  // the reference strips a trailing space
    if (!kept) return false;
    for (u32 i = 0; i < kept; ++i) {
        const char one[2] = {stem[i], 0};
        if (!append(out, size, n, one)) return false;
    }
    return append(out, size, n, suffix) && append(out, size, n, extension);
}
// The folder part of a path (everything before its last '/'), or "" for none.
inline void path_folder(const char* path, char* out, u32 size) {
    u32 last = 0, n = 0;
    bool any = false;
    for (u32 i = 0; path[i]; ++i)
        if (path[i] == '/') { last = i; any = true; }
    if (any)
        for (u32 i = 0; i < last && n + 1 < size; ++i) out[n++] = path[i];
    out[n] = 0;
}
inline bool starts_with(const char* text, const char* prefix) {
    for (; *prefix; ++prefix, ++text)
        if (*text != *prefix) return false;
    return true;
}
}}
