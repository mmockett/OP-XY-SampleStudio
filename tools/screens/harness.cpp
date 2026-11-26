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

// Behaviour the screens can't show, as the device saw it go wrong.
#define EXPECT(condition) do { if (!(condition)) { std::fprintf(stderr, "harness.cpp:%d: %s\n", __LINE__, #condition); std::exit(1); } } while (0)
bool shows_edit() { return !std::strcmp(output.texts[panel::T_M0].chars, "edit"); }
void file_mode_checks() {
    // A synth track's library, just opened on a file: stock hasn't recorded a highlight yet, but
    // the label is there on the first frame, and no frame asks stock's library tree (doing so from
    // the frame crashed the library as it opened).
    close_studio(false);
    studio.mode = kClosed;
    harness::engine = 0;
    harness::highlight_seen = false;
    harness::highlight = "";   // a folder highlighted
    harness::highlight_asks = 0;
    for (u32 i = 0; i < 20; ++i) {
        harness::clock += 100;
        EXPECT(frame(16) && shows_edit());
    }
    EXPECT(harness::highlight_asks == 0);
    // M1 on a folder asks for a sample and opens nothing; on a file it opens it
    press(kKeyM1, false);   // a folder: the popup asks for a sample, and the key goes on to stock
    EXPECT(studio.mode == kClosed);
    EXPECT(harness::highlight_asks == 1);
    // M1 opens the file; the track's own engine is held silent (B3 145 is channel 7) and a note plays the edit
    harness::highlight = "/fat32/samples/user/drum loop.wav";
    std::memset(harness::sent_on, 0, sizeof harness::sent_on);
    EXPECT(press(kKeyM1, false) && studio.mode == kOpen);
    EXPECT(studio.file_only && !studio.on_key);
    EXPECT(harness::sent_on[7] && harness::sent[7] == 1);
    studio.ready = true;   // the PCM in, as open_done leaves it: the whole file
    studio.edit.start = 0;
    studio.edit.end = kLoop;
    studio.opened = studio.edit;
    play.on = false;
    harness::notes += 1;
    harness::clock += 20;
    follow_notes(harness::clock);
    EXPECT(play.on && play.by_note);
    close_studio(false);
    EXPECT(harness::sent[7] == 0);   // the track sounds again
    std::printf("file mode (checked)\n");
}

// "This kit": the slices go on the kit's pads from the selected key up, in key order. A pad is a key
// with its own region or a key with no region at all (stock's new kit sets up only the pads it has
// loaded); a key on another key's region isn't one.
void kit_reset(const bool* loaded, u32 slot, u32 slices) {
    std::memset(harness::kit, 0, sizeof harness::kit);
    std::memset(harness::put, 0, sizeof harness::put);
    for (u32 k = 0; k < 24; ++k) {
        harness::has[k] = loaded[k];
        harness::alias[k] = -1;
    }
    harness::fold_into = -1;
    render.new_kit = false;
    render.engine = kDrumSampler;
    render.track = 3;
    render.slot = slot;
    render.cut_count = slices;
    for (u32 k = 0; k < slices; ++k) {
        std::snprintf(render.slice_paths[k], sizeof render.slice_paths[k], "/fat32/samples/user/break-s%u.wav",
                      static_cast<unsigned>(k + 1));
        render.slice_frames[k] = 1000 * (k + 1);
    }
    slices_to_kit = true;
    studio.on_key = true;
}
// The slice on each key, or 0 for none: the number in its file name.
u32 slice_on(u32 key) {
    const unsigned char* e = host::key_entry(3, key);
    if (!e) return 0;
    const char* name = host::entry_path(e);
    const char* s = std::strstr(name, "break-s");
    return s ? static_cast<u32>(std::atoi(s + 7)) : 0;
}
void kit_checks() {
    bool loaded[24];
    // a fresh kit with pad 1 loaded, and a full kit: slices 1..8 on keys 53..60, each at root 60, fine 0
    for (u32 full = 0; full < 2; ++full) {
        for (u32 k = 0; k < 24; ++k) loaded[k] = full || k == 0;
        kit_reset(loaded, 53, 8);
        load_kit();
        for (u32 k = 0; k < 24; ++k) EXPECT(harness::put[k] == (k < 8 ? 1u : 0u));
        for (u32 k = 0; k < 8; ++k) EXPECT(slice_on(53 + k) == k + 1);
        for (u32 k = 0; k < 8; ++k) EXPECT(harness::kit[k][24] == 60 && harness::kit[k][28] == 0);
        EXPECT(kit_undo.valid && kit_undo.count == 8);
    }
    // the one loaded pad on key 60, selected: slices on 60, 61, 62 ...
    for (u32 k = 0; k < 24; ++k) loaded[k] = k == 7;
    kit_reset(loaded, 60, 8);
    load_kit();
    for (u32 k = 0; k < 8; ++k) EXPECT(slice_on(60 + k) == k + 1);
    EXPECT(slice_on(54) == 0 && slice_on(59) == 0);
    // pads on 53 and 55 loaded, 53 selected: 54 is an empty pad of its own, slice 2 goes there
    for (u32 k = 0; k < 24; ++k) loaded[k] = k == 0 || k == 2;
    kit_reset(loaded, 53, 4);
    load_kit();
    for (u32 k = 0; k < 4; ++k) EXPECT(slice_on(53 + k) == k + 1);
    // keys sharing pad 1's region aren't pads: nothing goes there, and pad 1 keeps slice 1
    for (u32 k = 0; k < 24; ++k) loaded[k] = k == 0 || k >= 4;
    kit_reset(loaded, 53, 4);
    harness::alias[1] = harness::alias[2] = harness::alias[3] = 0;
    load_kit();
    EXPECT(slice_on(53) == 1 && harness::put[1] == 0 && slice_on(57) == 2);
    // stock putting an empty key's file into pad 1's region: pad 1 gets slice 1 back, the rest stay files
    for (u32 k = 0; k < 24; ++k) loaded[k] = k == 0;
    kit_reset(loaded, 53, 4);
    harness::fold_into = 0;
    load_kit();
    harness::fold_into = -1;
    EXPECT(slice_on(53) == 1 && kit_undo.count == 1);
    std::printf("this kit on fresh, partial and full kits (checked)\n");
}

// Earlier device faults, as behaviour.
void earlier_checks() {
    // "edit" sat over stock's arrow on a drum sampler: there it waits for Shift.
    close_studio(false);
    studio.mode = kClosed;
    harness::engine = opxy::studio::engine_drum;
    harness::shift = false;
    harness::clock += 1000;
    frame(16);
    EXPECT(!shows_edit());
    harness::shift = true;
    harness::clock += 100;
    EXPECT(frame(16) && shows_edit());
    harness::shift = false;
    harness::engine = opxy::studio::engine_sampler;
    // Pads 14 and up were silent with 24 slices under tape retime: every slice's source range lies
    // inside the source (the cuts are output frames, the live chain plays source frames).
    open_scene(kChop);
    studio.edit.start = 0;
    studio.edit.end = 86391;
    studio.edit.stretch = 172;
    studio.edit.beat = 0;
    studio.cut_count = 24;
    const u32 n = output_length();
    EXPECT(n > studio.edit.end);
    for (u32 k = 0; k <= 24; ++k) studio.cuts[k] = static_cast<u32>(static_cast<unsigned long long>(n) * k / 24);
    for (u32 k = 1; k <= 24; ++k) {
        u32 a = 0, b = 0;
        slice_source(studio.edit, k, a, b);
        EXPECT(a < b && b <= studio.edit.end);
    }
    // A save lists itself in the library once the studio closes.
    harness::relists = 0;
    library_stale = true;
    studio.mode = kOpen;
    close_studio(false);
    EXPECT(harness::relists == 1);
    // A new kit is in stock's preset index as soon as it is written, with the studio still open: once
    // the library leaves the screen the studio sees no frame, so it can't wait for a close.
    open_scene(kChop);
    harness::rescans = 0;
    render.new_kit = true;
    render.cut_count = 8;
    copy_text(render.kit_folder, "/fat32/presets/snapshot/break.preset", sizeof render.kit_folder);
    load_kit();
    EXPECT(studio.mode == kOpen && harness::rescans == 1);
    render.new_kit = false;
    // Turns at positions the library reports past the four encoders: taken while the studio is open
    // (stock's list stays put under it) and acting on nothing; stock's while it is closed.
    open_scene(kTrim);
    const Edit before = studio.edit;
    EXPECT(turn_encoder(5, 1) && turn_encoder(9, -2));
    EXPECT(same_edit(studio.edit, before));
    close_studio(false);
    EXPECT(!turn_encoder(5, 1));
    std::printf("earlier device faults (checked)\n");
}

// Leaving the library for another browser on the same screen (the preset browser) without closing
// the studio. The SDK hands the editor that browser's frames and draws stock's whatever the editor
// returns (sample_studio_host.cpp, tests/test_sample_studio.py there); here, the editor's side: it
// closes at once, and the close does its work. 029 and 030 skipped this frame, so these never ran.
void leave_checks() {
    // a save listed in the library, and the track's own engine sounding again
    open_scene(kTrim);
    harness::off_top = false;
    harness::relists = 0;
    library_stale = true;
    std::memset(harness::sent_on, 0, sizeof harness::sent_on);
    harness::off_top = true;
    harness::clock += 16;
    EXPECT(!frame(16));                       // the editor draws nothing over the preset browser
    EXPECT(studio.mode == kClosed);
    EXPECT(harness::relists == 1);
    EXPECT(harness::sent_on[7] && harness::sent[7] == 0);
    // a new kit: in stock's preset index when written, and the studio closes as the preset browser opens
    harness::off_top = false;
    open_scene(kChop);
    harness::rescans = 0;
    render.new_kit = true;
    render.cut_count = 8;
    copy_text(render.kit_folder, "/fat32/presets/snapshot/break.preset", sizeof render.kit_folder);
    load_kit();
    render.new_kit = false;
    EXPECT(harness::rescans == 1);
    harness::off_top = true;
    harness::clock += 16;
    EXPECT(!frame(16) && studio.mode == kClosed);
    harness::off_top = false;
    std::printf("leaving for another browser (checked)\n");
}
}}}}}  // namespace opxy::extensions::sample_studio::edit::scenes

int main(int argc, char** argv) {
    opxy::extensions::sample_studio::edit::scenes::draw_all(argc > 1 ? argv[1] : ".");
    opxy::extensions::sample_studio::edit::scenes::file_mode_checks();
    opxy::extensions::sample_studio::edit::scenes::kit_checks();
    opxy::extensions::sample_studio::edit::scenes::earlier_checks();
    opxy::extensions::sample_studio::edit::scenes::leave_checks();
    return 0;
}
