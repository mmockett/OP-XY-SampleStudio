// Sample Studio's screens, drawn by its own code on Linux (Dockerfile): the SDK's sample_studio.cpp
// with stock calls stubbed (stock_host.hpp) and stock memory as zeroed pages, each scene's state
// set by hand, then page() fills the retained frame, written out raw for render.py.
#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "sample_studio.cpp"

extern "C" {
// The library's own handlers the studio falls back to; never reached here.
u32 sdk_sample_studio_stock_press(unsigned char*, const u32*) { return 0; }
u32 sdk_sample_studio_stock_shift_press(unsigned char*, const u32*) { return 0; }
u32 sdk_sample_studio_stock_release(unsigned char*, u32) { return 0; }
void sdk_sample_studio_stock_turn(unsigned char*, u32, const i32*) {}
void sdk_sample_studio_stock_frame(void*, i32) {}
}
const u32 sdk_sample_studio_label __asm__("sdk_sample_studio_label") = 1;
// Other platform units' services the studio calls; never reached in a drawn frame.
u32 sdk_sample_ui_engine(u32) __asm__("sdk_sample_ui_engine");
u32 sdk_sample_ui_engine(u32) { return 0; }
u32 sdk_sample_cache_stock_hash(const char*) __asm__("sdk_sample_cache_stock_hash");
u32 sdk_sample_cache_stock_hash(const char*) { return 0; }
void sdk_sample_cache_sample_release(u32) __asm__("sdk_sample_cache_sample_release");
void sdk_sample_cache_sample_release(u32) {}


namespace opxy { namespace platform { bool harness_shift; }}

namespace {
// A two-bar drum loop at 120 BPM, synthesised here: kick, snare, hats. 44.1 kHz stereo.
const u32 kRate = 44100, kLoop = kRate * 4;
opxy::i16 loop_pcm[kLoop * 2];
u32 noise_state = 0x12345678u;
i32 noise() { noise_state = noise_state * 1664525u + 1013904223u; return static_cast<i32>(noise_state >> 16) - 32768; }
void make_loop() {
    // Integer-only synthesis, so every build draws the same screens: a kick (falling triangle), a
    // snare (noise), eighth-note hats, and a bass line under it all.
    std::memset(loop_pcm, 0, sizeof loop_pcm);
    const u32 beat = kRate / 2;   // 120 BPM
    const u32 kick_at[5] = {0, 3 * beat / 2, 4 * beat, 5 * beat + beat / 2, 6 * beat};
    const u32 snare_at[4] = {beat, 3 * beat, 5 * beat, 7 * beat};
    for (u32 f = 0; f < kLoop; ++f) {
        i32 v = 0;
        for (u32 k = 0; k < 5; ++k) {
            if (f < kick_at[k] || f - kick_at[k] >= kRate * 2 / 5) continue;
            const u32 i = f - kick_at[k];
            const i32 decay = static_cast<i32>(kRate * 2 / 5 - i) * 1000 / static_cast<i32>(kRate * 2 / 5);
            const u32 period = 300 + i / 40;   // the pitch falls
            const u32 ph = i % period;
            const i32 tri = ph < period / 2 ? static_cast<i32>(ph * 4000 / period) - 1000 : 3000 - static_cast<i32>(ph * 4000 / period);
            v += tri * 24 * decay / 1000;
        }
        for (u32 k = 0; k < 4; ++k) {
            if (f < snare_at[k] || f - snare_at[k] >= kRate / 4) continue;
            const u32 i = f - snare_at[k];
            const i32 decay = static_cast<i32>(kRate / 4 - i) * 1000 / static_cast<i32>(kRate / 4);
            v += noise() * decay / 1000 * 2 / 3;
        }
        const u32 in8 = f % (beat / 2);
        if (in8 < kRate / 16) v += noise() * static_cast<i32>(kRate / 16 - in8) / static_cast<i32>(kRate / 16) / 5;
        const u32 bass_period = (f / (2 * beat)) % 2 ? 802 : 1070;   // two notes a bar
        const u32 bp = f % bass_period;
        const i32 saw = static_cast<i32>(bp * 2000 / bass_period) - 1000;
        v += saw * 6;
        if (v > 32000) v = 32000;
        if (v < -32000) v = -32000;
        loop_pcm[2 * f] = static_cast<opxy::i16>(v);
        loop_pcm[2 * f + 1] = static_cast<opxy::i16>(v * 9 / 10);
    }
}
// The envelope the studio keeps (open_job's loop).
void load_loop() {
    studio.view.data = loop_pcm;
    studio.view.frames = kLoop;
    studio.view.rate = kRate;
    studio.view.channels = 2;
    u32 peak = 0;
    for (u32 b = 0; b < kBins; ++b) {
        const u32 f0 = b * kLoop / kBins, f1 = (b + 1) * kLoop / kBins;
        u32 m = 0;
        for (u32 f = f0; f < f1; ++f)
            for (u32 c = 0; c < 2; ++c) {
                const i32 v = loop_pcm[f * 2 + c];
                const u32 a = static_cast<u32>(v < 0 ? -v : v);
                if (a > m) m = a;
            }
        studio.env[b] = static_cast<unsigned short>(m > 32767 ? 32767 : m);
        if (m > peak) peak = m;
    }
    studio.peak = peak;
}
void open_scene(Page page, u32 row = 0) {
    std::memset(&header_state, 0, sizeof header_state);
    axis_moving = false;   // the output's length eases on the device; here each scene starts settled
    axis_frames = 0;
    clear_card();
    opxy::platform::harness_shift = false;
    studio.mode = kOpen;
    studio.page = page;
    studio.row = row;
    studio.track = 0;
    studio.engine = kSampler;
    studio.ready = true;
    copy_text(studio.path, "/fat32/samples/user/drum loop.wav", sizeof studio.path);
    Edit e = {};
    neutral(e);
    e.start = 0;
    e.end = kLoop;
    e.level = 0;
    studio.edit = e;
    studio.opened = e;
    studio.out_valid = false;
    studio.rendered = 0;
    studio.change = 1;
    studio.showing_preview = false;
    play.on = false;
    ++studio.generation;
}
// A play of the original from `start`, `seconds` in.
void playing(u32 start, u32 end, u32 ms) {
    play.on = true;
    play.original = true;
    play.live = false;
    play.growing = false;
    play.reverse = false;
    play.start = start;
    play.end = end;
    play.rate = kRate;
    play.t0 = 0u - ms;
}
void write(const char* folder, const char* name) {
    page();
    char path[256];
    std::snprintf(path, sizeof path, "%s/%s.bin", folder, name);
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); std::exit(1); }
    std::fwrite(&output, 1, sizeof output, f);
    std::fclose(f);
    std::printf("%s\n", name);
}
// Stock's memory as zeroed pages, where the studio reads it (selected track, note map, globals).
void map(u32 at, u32 bytes) {
    void* p = mmap(reinterpret_cast<void*>(static_cast<uintptr_t>(at)), bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
}
}  // namespace

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : ".";
    map(0x08000000u, 0x01000000u);
    map(0x84000000u, 0x03000000u);
    map(0x11900000u, 0x00100000u);
    make_loop();
    load_loop();

    // Trim: the markers in a little, fades, playing.
    open_scene(kTrim);
    studio.edit.start = kRate / 4;
    studio.edit.end = kLoop - kRate / 8;
    studio.edit.fade_in = 10;
    studio.edit.fade_out = 250;
    playing(studio.edit.start, studio.edit.end, 1400);
    write(out, "01-trim");

    // Trim, row 2, reversed: the waveform mirrored, the playhead running left to right.
    open_scene(kTrim, 1);
    studio.edit.start = kRate / 4;
    studio.edit.end = kLoop - kRate / 8;
    studio.edit.reverse = 1;
    studio.edit.level = -3;
    playing(studio.edit.start, studio.edit.end, 1100);
    play.reverse = true;
    write(out, "02-trim-reverse");

    // Slice: transients, slice 3 chosen.
    open_scene(kChop);
    studio.edit.slices = 8;
    studio.edit.chop_mode = 1;
    studio.edit.slice = 3;
    chop_cuts();
    write(out, "03-slice");

    // Retime: tape, three quarters speed.
    open_scene(kTime);
    studio.edit.beat = 0;
    studio.edit.stretch = 133;
    write(out, "04-retime");

    // Process: rate, bits, drive and the low-pass.
    open_scene(kColor);
    studio.edit.rate_index = 3;
    studio.edit.bits_index = 3;
    studio.edit.drive = 30;
    studio.edit.sweep = -20;
    write(out, "05-process");

    // Process, row 2: the sends, a full tail, comp.
    open_scene(kColor, 1);
    studio.edit.rate_index = 3;
    studio.edit.bits_index = 3;
    studio.edit.drive = 30;
    studio.edit.sweep = -20;
    studio.edit.send1 = 40;
    studio.edit.send2 = 16;
    studio.edit.tail = kTailFull;
    studio.edit.comp = 30;
    write(out, "06-process-sends");

    // Shift held: undo, compare, play, save.
    open_scene(kTrim);
    opxy::platform::harness_shift = true;
    write(out, "07-shift");

    // Save slices on a drum track: where they go.
    open_scene(kChop);
    studio.engine = kDrumSampler;
    studio.edit.slices = 8;
    chop_cuts();
    studio.mode = kSliceChoice;
    set_card("save slices", "new kit, files, kit", 0);
    write(out, "08-save-slices");

    // A save through the effects, recorded live.
    open_scene(kColor, 1);
    studio.edit.send1 = 40;
    studio.edit.tail = kTailFull;
    studio.mode = kSaving;
    set_card("printing the fx live", "35 s max; sample stops", 0);
    write(out, "09-printing");
    return 0;
}
