// Sample Studio's screens, drawn by its own code on Linux (Dockerfile): the extension's editor
// against stand-in SDK services (services.cpp), each scene's state set by hand, then page() fills
// the retained frame, written out raw for render.py.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "services.cpp"
#include "editor.cpp"

namespace opxy { namespace extensions { namespace sample_studio { namespace edit { namespace scenes {
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
    harness::shift = false;
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
// The SDK draws a frame only when the slots the artwork bound pass draw::valid's rules
// (sample_studio_host.cpp, bound_valid); a frame that fails is dropped on the device, so fail here.
bool drawable(const Frame& f) {
    using namespace opxy::draw;
    for (u32 p = 0; p < panel::kPaths; ++p) {
        if (!f.paths[p].key) return false;
        for (u32 i = 0; i < panel::kPoints; ++i)
            if (f.paths[p].points[i].x < -path_extent * unit || f.paths[p].points[i].x > path_extent * unit ||
                f.paths[p].points[i].y < -path_extent * unit || f.paths[p].points[i].y > path_extent * unit) return false;
    }
    for (u32 i = 0; i < panel::kNodes; ++i)
        if (!placement(f.nodes[i].x, f.nodes[i].y, f.nodes[i].scale) || !rotation_valid(f.nodes[i].cos, f.nodes[i].sin) ||
            f.nodes[i].shade >= panel::kPalette || f.nodes[i].paint > fill_and_stroke) return false;
    for (u32 i = 0; i < panel::kTexts; ++i)
        if (!placement(f.texts[i].x, f.texts[i].y, f.texts[i].scale) || !terminated(f.texts[i].chars, text_capacity)) return false;
    for (u32 i = 0; i < page_controls; ++i)
        if (!terminated(f.header[i], header_capacity)) return false;
    return true;
}
void write(const char* folder, const char* name) {
    page();
    if (!drawable(output)) { std::fprintf(stderr, "%s: the SDK would not draw this frame\n", name); std::exit(1); }
    char path[256];
    std::snprintf(path, sizeof path, "%s/%s.bin", folder, name);
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror(path); std::exit(1); }
    std::fwrite(&output, 1, sizeof output, f);
    std::fclose(f);
    std::printf("%s\n", name);
}
void draw_all(const char* out) {
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
    harness::shift = true;
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

    // The closed studio's frame over the library ("edit" over M1): checked, not rendered.
    studio.mode = kClosed;
    harness::shift = false;
    label();
    if (!drawable(output)) { std::fprintf(stderr, "label: the SDK would not draw this frame\n"); std::exit(1); }
    std::printf("label (checked)\n");
}
}}}}}  // namespace opxy::extensions::sample_studio::edit::scenes

int main(int argc, char** argv) {
    opxy::extensions::sample_studio::edit::scenes::draw_all(argc > 1 ? argv[1] : ".");
    return 0;
}
