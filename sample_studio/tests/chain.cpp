// Sample Studio's render chain and save helpers (studio_dsp.hpp, bake.hpp), as the device compiles
// them: an untouched edit gives the source back, reverse and tape speed give the frames and lengths
// they should, the block form makes the same words as the frame form, and a save's gain, header
// and path come out right. Returns 0 when every check passes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "studio_dsp.hpp"

using namespace opxy;
using namespace opxy::studio_dsp;

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)

namespace {
const u32 kRate = 44100, kFrames = 3000;
i16 pcm[kFrames * 2];

// A tone with a fast partial on each channel, integer only so every compiler makes the same one.
void make_tone(u32 channels) {
    u32 state = 0x2545f491u;
    for (u32 i = 0; i < kFrames; ++i)
        for (u32 c = 0; c < channels; ++c) {
            state = state * 1664525u + 1013904223u;
            const i32 ramp = static_cast<i32>((i * (7u + 3u * c)) % 400u) * 80 - 16000;
            const i32 noise = static_cast<i32>(state >> 20) - 2048;
            pcm[i * channels + c] = static_cast<i16>(ramp + noise);
        }
}
Settings plain() {
    Settings s;
    std::memset(&s, 0, sizeof s);
    s.begin = 100;
    s.end = kFrames - 100;
    s.gain = sample_bake::kUnity;
    s.stretch = 100;
    s.grain_ms = 40;
    return s;
}
// Every frame the frame form makes, into out (two words a frame); how many.
u32 frames_of(const Settings& s, u32 channels, i32* out, u32 capacity) {
    static Pipeline p;
    p.start(pcm, channels, kRate, s);
    u32 n = 0;
    i32 w[2];
    while (n < capacity && p.next(w)) {
        out[2 * n] = w[0];
        out[2 * n + 1] = w[1];
        ++n;
    }
    return n;
}

const u32 kCapacity = kFrames * 5;
i32 frame_words[kCapacity * 2];

void an_untouched_edit_is_the_source() {
    for (u32 channels = 1; channels <= 2; ++channels) {
        make_tone(channels);
        const Settings s = plain();
        CHECK(!time_active(s) && !color_active(s));
        const u32 n = frames_of(s, channels, frame_words, kCapacity);
        CHECK(n == s.end - s.begin);
        for (u32 i = 0; i < n; ++i)
            for (u32 c = 0; c < 2; ++c)
                CHECK(frame_words[2 * i + c] == pcm[(s.begin + i) * channels + (channels > 1 ? c : 0)] * 65536);
    }
}

void reverse_reads_from_the_end() {
    make_tone(2);
    Settings s = plain();
    s.reverse = 1;
    const u32 n = frames_of(s, 2, frame_words, kCapacity);
    CHECK(n == s.end - s.begin);
    for (u32 i = 0; i < n; ++i)
        CHECK(frame_words[2 * i] == pcm[(s.end - 1 - i) * 2] * 65536);
}

void tape_speed_sets_the_length() {
    make_tone(1);
    const u32 source = plain().end - plain().begin;
    const u32 stretches[] = {50, 200, 400};
    for (u32 k = 0; k < 3; ++k) {
        Settings s = plain();
        s.stretch = stretches[k];
        const u32 n = frames_of(s, 1, frame_words, kCapacity);
        const u32 want = source * s.stretch / 100;
        CHECK(n + 4 >= want && n <= want + 4);
    }
    // tape ties pitch to the stretch: the pitch field leaves the length as it is
    Settings tied = plain();
    tied.pitch = 12;
    CHECK(frames_of(tied, 1, frame_words, kCapacity) == source);
    CHECK(kSemitoneQ16[24] == 65536u && kSemitoneQ16[36] == 131072u && kSemitoneQ16[12] == 32768u);
}

void beat_sets_the_length_and_the_pitch_apart() {
    make_tone(2);
    const u32 source = plain().end - plain().begin;
    struct Case { u32 stretch; i32 pitch; u32 want; };
    const Case cases[] = {{150, 0, source * 3 / 2}, {100, 12, source}, {100, -7, source}, {60, 5, source * 3 / 5}};
    for (u32 k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
        Settings s = plain();
        s.beat = 1;
        s.stretch = cases[k].stretch;
        s.pitch = cases[k].pitch;
        const u32 n = frames_of(s, 2, frame_words, kCapacity);
        const u32 grain = kRate * grain_ms_for(s) / 1000;
        CHECK(n + grain >= cases[k].want && n <= cases[k].want + grain);
    }
}

void the_block_form_makes_the_same_words() {
    struct Case { u32 drive, bits, rate, comp, dc, stretch, reverse; i32 sweep; };
    const Case cases[] = {{0, 0, 0, 0, 0, 100, 0, 0}, {100, 0, 0, 0, 0, 100, 0, 0}, {0, 6, 0, 0, 0, 100, 0, 0},
                          {0, 0, 7, 0, 0, 100, 0, 0}, {0, 0, 0, 0, 0, 100, 0, -40}, {0, 0, 0, 0, 0, 100, 0, 20},
                          {0, 0, 0, 90, 0, 100, 0, 0}, {0, 0, 0, 0, 1, 100, 0, 0}, {0, 0, 0, 0, 0, 40, 0, 0},
                          {0, 0, 0, 0, 0, 250, 1, 0}, {70, 5, 4, 60, 1, 130, 0, -12}};
    static i32 l[kCapacity], r[kCapacity];
    for (u32 channels = 1; channels <= 2; ++channels) {
        make_tone(channels);
        for (u32 k = 0; k < sizeof cases / sizeof cases[0]; ++k) {
            Settings s = plain();
            s.drive = cases[k].drive;
            s.bits_index = cases[k].bits;
            s.rate_index = cases[k].rate;
            s.comp = cases[k].comp;
            s.dc = cases[k].dc;
            s.stretch = cases[k].stretch;
            s.reverse = cases[k].reverse;
            s.sweep = cases[k].sweep;
            const u32 n = frames_of(s, channels, frame_words, kCapacity);
            static Pipeline block;
            block.start(pcm, channels, kRate, s);
            u32 got = 0;
            for (u32 made = 1; made && got < kCapacity;) {
                const u32 want = got + 64 <= kCapacity ? 64 : kCapacity - got;
                made = block.fill(l + got, r + got, want);
                got += made;
            }
            CHECK(got == n);
            for (u32 i = 0; i < n; ++i) {
                if (l[i] != frame_words[2 * i] || r[i] != frame_words[2 * i + 1])
                    std::fprintf(stderr, "channels %u, case %u, frame %u\n", static_cast<unsigned>(channels),
                                 static_cast<unsigned>(k), static_cast<unsigned>(i));
                CHECK(l[i] == frame_words[2 * i]);
                CHECK(r[i] == frame_words[2 * i + 1]);
            }
        }
    }
}

// A note's transposition (editor.cpp A48, player.cpp P16): the ratio, its products and the frame
// counts it scales against 64-bit arithmetic, and the tape stage at a transposed speed: an octave up
// plays every other source frame, so half as many, each the source's own.
void a_note_transposes_in_the_tape_stage() {
    CHECK(transpose_q16(0) == 65536u && transpose_q16(1200) == 131072u && transpose_q16(-1200) == 32768u);
    CHECK(transpose_q16(2400) == 262144u && transpose_q16(-2400) == 16384u && transpose_q16(9000) == 262144u);
    u32 last = 0;
    for (i32 c = -2400; c <= 2400; c += 5) {
        const u32 r = transpose_q16(c);
        CHECK(r > last);   // rises with every 5 cents
        last = r;
    }
    // 50 cents: 2^(1/24) = 1.029302 (Q16 67455)
    CHECK(transpose_q16(50) >= 67450u && transpose_q16(50) <= 67460u);
    const u32 as[] = {1u, 65536u, 285000u, 4000000u}, bs[] = {16384u, 65536u, 98304u, 262144u, 300000u};
    for (u32 i = 0; i < 4; ++i)
        for (u32 j = 0; j < 5; ++j) {
            const unsigned long long want = static_cast<unsigned long long>(as[i]) * bs[j] >> 16;
            CHECK(mul_q16(as[i], bs[j]) == static_cast<u32>(want));
        }
    const u32 ns[] = {0u, 1u, 7469u, 95932u, 4000000u};
    for (u32 i = 0; i < 5; ++i)
        for (u32 j = 0; j < 5; ++j) {
            const unsigned long long want = (static_cast<unsigned long long>(ns[i]) << 16) / bs[j];
            CHECK(frames_over(ns[i], bs[j]) == static_cast<u32>(want));
        }
    make_tone(1);
    Settings s = plain();
    static Pipeline p;
    p.start(pcm, 1, kRate, s);
    p.speed_q16 = mul_q16(p.speed_q16, transpose_q16(1200));
    i32 w[2];
    u32 n = 0;
    while (p.next(w)) {
        CHECK(w[0] == pcm[s.begin + 2 * n] * 65536);
        ++n;
    }
    const u32 source = s.end - s.begin;
    CHECK(n + 2 >= source / 2 && n <= source / 2 + 1);
    CHECK(frames_over(source, transpose_q16(1200)) == source / 2);
}

// A file's own root (editor.cpp A49): the first well-formed 'smpl' chunk, read through the chunks.
struct MemoryReader {
    const unsigned char* data;
    u32 size, at;
    u32 operator()(void* to, u32 n) {
        const u32 k = at + n <= size ? n : size - at;
        std::memcpy(to, data + at, k);
        at += k;
        return k;
    }
};
void a_files_root_comes_from_its_smpl_chunk() {
    using namespace opxy::sample_bake;
    unsigned char f[200] = {}, scratch[64];
    std::memcpy(f, "RIFF\0\0\0\0WAVEdata", 16);
    put32(f + 16, 5);                       // an odd chunk: padded to 6
    std::memcpy(f + 26, "smpl", 4);
    put32(f + 30, 36);
    put32(f + 34 + 12, 36);                 // unity C2
    put32(f + 34 + 16, 0x40000000u);        // a quarter semitone above
    MemoryReader r = {f, 70, 0};
    i32 cents = 0;
    CHECK(wav_root_cents(r, cents, 1u << 20, scratch, sizeof scratch) && cents == 3625);
    MemoryReader capped = {f, 70, 0};
    CHECK(!wav_root_cents(capped, cents, 30, scratch, sizeof scratch));   // the limit reached first
    put32(f + 34 + 12, 128);                // past MIDI's notes: no root
    MemoryReader bad = {f, 70, 0};
    CHECK(!wav_root_cents(bad, cents, 1u << 20, scratch, sizeof scratch));
    MemoryReader none = {f, 26, 0};          // the file ends before any smpl
    CHECK(!wav_root_cents(none, cents, 1u << 20, scratch, sizeof scratch));
}

void a_save_normalises_and_names_itself() {
    using namespace opxy::sample_bake;
    CHECK(normalise_gain(0, kTarget) == kUnity);                   // silence: no gain
    CHECK(normalise_gain(kTarget, kTarget) == kUnity);             // already at -1 dBFS
    CHECK(normalise_gain(1, kTarget) == kGainCap);                 // capped
    const u32 half = normalise_gain(2 * kTarget, kTarget);
    CHECK(half == kUnity / 2);
    CHECK(level_gain(0) == kUnity);
    for (i32 db = kLevelMin + 1; db <= kLevelMax; ++db) CHECK(level_gain(db) > level_gain(db - 1));

    unsigned char h[kHeaderBytes];
    wav_header(h, 1000, 2, 48000);
    CHECK(!std::memcmp(h, "RIFF", 4) && !std::memcmp(h + 8, "WAVEfmt ", 8) && !std::memcmp(h + 36, "data", 4));
    CHECK(h[22] == 2 && h[24] == 0x80 && h[25] == 0xbb && h[32] == 4 && h[34] == 16);
    CHECK(h[40] == (4000 & 0xff) && h[41] == (4000 >> 8) && h[4] == ((36 + 4000) & 0xff));

    char stem[64], path[128];
    path_stem("/fat32/samples/user/loops/Big Loop #2.wav", stem, sizeof stem);
    CHECK(!std::strcmp(stem, "Big Loop #2"));
    CHECK(edit_path(path, sizeof path, "/fat32/samples/user/loops", stem, 7, ".wav"));
    CHECK(!std::strcmp(path, "/fat32/samples/user/loops/Big Loop #2-ed7.wav"));
    // a long stem is shortened to keep the path within stock's 95 characters; the folder never is
    const char* folder = "/fat32/samples/user/a folder with a long name/and another one inside";
    char long_stem[64];
    std::memset(long_stem, 'x', 60);
    long_stem[60] = 0;
    CHECK(edit_path(path, sizeof path, folder, long_stem, 12, ".wav"));
    CHECK(std::strlen(path) == 95 && !std::strncmp(path, folder, std::strlen(folder)));
    CHECK(!std::strcmp(path + 95 - 9, "-ed12.wav"));

    DcBlock dc;
    dc.start(44100);
    i32 y = 0;
    for (u32 i = 0; i < 44100; ++i) y = dc.process(0, 1 << 28);   // a constant input decays toward 0
    CHECK(y >= 0 && y < (1 << 28) / 100);
}
}  // namespace

int main() {
    an_untouched_edit_is_the_source();
    reverse_reads_from_the_end();
    tape_speed_sets_the_length();
    beat_sets_the_length_and_the_pitch_apart();
    the_block_form_makes_the_same_words();
    a_note_transposes_in_the_tape_stage();
    a_files_root_comes_from_its_smpl_chunk();
    a_save_normalises_and_names_itself();
    std::printf("Sample Studio chain: all checks passed\n");
    return 0;
}
