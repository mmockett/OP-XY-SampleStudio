// Sample Studio's editor, in stock's sample library, through the SDK's sample_studio
// capability (opxy/sample_studio.hpp, the SDK's docs/sample-studio.md).
//
// The way in: the sample library (Shift + Sample) shows "edit" over M1 on a
// sampler track, and on a drum sampler or multisampler track while Shift is
// held (M1 is stock's previous key there). M1 (sampler) or Shift + M1 (any of the three)
// opens the studio on the key's sample, the file the library has just put there.
//
// The studio is a page of the library: the SDK hands it the library controller's
// press, Shift press, release and turn, and the browser screen's frame, so
// stock's list never moves under it.
//   M1..M4        the pages: trim, slice, retime, process
//   encoders      the bright header row; an encoder press swaps the rows
//   Shift held    M labels undo, compare, play, save (no page has a Shift row)
//   Shift + M1    undo; after a save, back to the original file
//   Shift + M2    compare, held: the key plays the original
//   Shift + M4    save: <stem>-ed<N>.wav beside the original, loaded on the key;
//                 on the slice page, the slices <stem>-s<k>.wav (on a drum
//                 sampler, loaded on the keys from this one up)
//   Sample        back to the library; with an unsaved edit, a card asks first
// What you hear: the studio's own player on the audio board, with the track
// silent: the edit live through the render's chain (A24), its sends live into
// FX I and FX II (A25); beat-mode retime renders a preview first (A23).
//
// Every choice a device run has not confirmed is under "Assumptions", each in
// one place, so it can be reversed alone.
#include <opxy/sample_studio.hpp>
#include <opxy/page_header.hpp>
#include "studio_dsp.hpp"
#include "panel.hpp"

namespace opxy { namespace extensions { namespace sample_studio { namespace edit {
using opxy::i32;
using opxy::u32;
using namespace opxy::sample_bake;
namespace dsp = opxy::studio_dsp;
namespace panel = opxy::extensions::sample_studio::studio_panel;
namespace draw = opxy::draw;
namespace host = opxy::studio;
// The SDK's services (opxy/sample_studio.hpp) under the names this file has always used.
using host::barrier;
using host::now_ms;
using host::File;
using host::file_open;
using host::file_write;
using host::file_read;
using host::file_close;
using host::rename_file;
using host::remove_file;
using host::make_folder;
using host::file_exists;
using host::allocate;
using host::release;
using host::popup;
using host::highlighted;
using host::library_on_top;

namespace {
// ---------------------------------------------------------------- Assumptions
// A1 Keys: the library controller's methods see every key
//    and encoder while the library is on top: press (ctl, event) with event[0]
//    the key, Shift press likewise, release (ctl, key), turn (ctl, position,
//    event) with event[1] the detents. A method that returns 1 keeps the key
//    from the global handlers. Encoder presses arrive as keys 10..13.
const u32 kKeyM1 = host::key_m1, kKeyM2 = host::key_m2, kKeyM3 = host::key_m3, kKeyM4 = host::key_m4,
          kKeyClick0 = host::key_click0, kKeyClick3 = host::key_click3, kKeySample = host::key_sample;
// A2 The library controller, its guards (rename editor ctl+0x50, delete prompt
//    ctl+0x5c) and the controller stack (0x0801ed24: five pointers, count at
//    +0x14), whose top is the library while it shows.
// A3 Drawing: the browser screen's frame (8d55341c) paints the studio in place
//    of stock's library while the studio is open; while it isn't, stock's
//    library and then the label over it.
// A4 Live preview: start, end, loop markers kept inside them, reverse and level
//    written to the key's pad record, the GUI's region change counter bumped,
//    and the region table sent (8d58938c), as stock's slicer does after it
//    rewrites regions (8c0a570e). At most every kFlushMs.
const u32 kFlushMs = 40;
// A5 Writing: from the main board through newlib, in whole 32 KiB writes (16 mono), at most
//    kJobFrames output frames a file-worker job, each job queued from the last
//    one's completion; a save goes through a .tmp renamed when complete, removed
//    on failure.
const u32 kChunkFrames = 8192;        // 32 KB writes of stereo (8 KB writes ran ~0.5 MB/s)
const u32 kJobFrames = 32768;
// A6 Putting a file on a key: stock's library completion order (8c0a2354): path
//    (8d5893bc), region initializer (8d589b8c sampler/multisampler, 8d589ac0
//    drum sampler), region table (8d58938c). Root kept; loop points -1, -1 (the
//    initializer puts them at 0.2 and 0.8 of the frames); level 0.
// A7 Undo after a save: the 128-byte region entry saved before loading,
//    written back, the counter bumped, the table sent again.
// A8 Paths: user samples absolute (/fat32/...), factory ones "content/samples/...";
//    a save of a factory sample goes to /fat32/samples/user.
const char kUserFolder[] = "/fat32/samples/user";
// A9 The PCM: the key's sample, borrowed from stock's cache on the file worker
//    while the studio is open, read on the GUI for the waveform, snap and trim.
// A10 The library has just opened when its frame runs after a gap of more than
//    kGapMs (or after another screen): then the key is snapshotted before any
//    audition resets its markers.
const u32 kGapMs = 400;
// A11 The rendered preview: a new file each render, /fat32/samples/user/.studio/
//    t<track>-<serial>.wav, the serial counting up from boot and never reused.
//    Stock's cache knows a sample by its path's hash alone, so a name used twice
//    can play an older render (a cycle of 16 names did).
//    The folder is made with bounce phase 1's mkdir (0x8c0bd5bc). Stock's library is
//    assumed to skip a folder whose name starts with a dot (U). Files no longer on
//    the key are removed once the next one is in the cache (A13), the rest when the
//    studio closes; the key gets the original back then, so a project never keeps one.
const char kScratchFolder[] = "/fat32/samples/user/.studio";
const u32 kDebounceMs = 200;          // an edit rests this long before it renders
const u32 kSweepMax = 64;            // files one removal job tries at most
// A13 Stock loads a file put on a key in the background, and the key is silent
//    until its cache holds it. After a preview goes on the key the studio asks the
//    cache (acquire and release on the file worker) every kCheckMs, says "loading
//    the render" meanwhile and starts no other render; at kResendMs it sends the
//    region table once more, and at kGiveUpMs it stops waiting. A failed preview
//    render is tried again after kRetryMs, kRetries times for the same edit.
const u32 kCheckMs = 100, kResendMs = 2500, kGiveUpMs = 8000;
const u32 kRetryMs = 1000, kRetries = 3;
// A14 Borrowing the PCM: stock's acquire (8d509d70) refuses a sample its cache
//    worker has marked unwanted (node +0x1c, stored at 8d50bd58 when the key is
//    missing from the desired list), and while the library is open the key's own
//    sample is unwanted: a borrow through it fails every time (78 in 78). So the
//    studio borrows as stock does, without that one test: manager+4 locked
//    (8d5be032 / 8d5be060), the node found (8d509824 on the map at 0x08039f24, its
//    end at +4), loaded (+0x1d), its object ready ([obj] and [obj+0x30] nonzero) and
//    its count (+0x18) raised. Stock's release (8d509df8) lowers it; a sample with a
//    count is never evicted (8d50bc58 runs only without references).
// A15 Stock gathers the keys' files (and so loads a new one, on both boards) only
//    when its "samples changed" byte 0x840a052d is set: its refresh (8d593594)
//    builds the track list, starts the gather job (8d4d8344, gather 8d4dadec) and
//    clears the byte. Stock sets it wherever a key's file changes: its region update
//    right after the table send (8d58956c, then 8d589572) and the library's own
//    handlers (8c0a25d6, 8c0a306a); without it a render loads
//    only at the next encoder turn and compare changes nothing. The studio sets it
//    after every table send that can change a key's file.
// A16 While the studio is open no region of the track changes: the raw sample is edited
//    in isolation and the key stays strictly untouched. Previews are files the
//    studio streams to its own player (A19); stock loads nothing for them. Save is the only
//    change to the key: it puts the saved file there, with undo.
// A17 Long edits: a render is cancelled at its next job once a newer edit arrives
//    (otherwise most renders, 27 of 45 measured, finish only to be thrown away). When the output would
//    run past kQuickMinMs, its first kQuickMs go on the key first (no count pass, a
//    short fade at the cut), then the whole edit renders and replaces it.
const u32 kQuickMs = 1500, kQuickMinMs = 3000, kQuickCutMs = 5;
// A19 Playback is the studio's own player on the audio board (player.cpp): Shift
//    + M3 plays the edit from its start (the rendered preview once it exists, its quick first
//    part meanwhile, or the original between the markers when nothing needs a render) and
//    stops; Shift + M2 held plays the original as it was when the studio opened. Commands go as
//    stock B3 parameters 138..144 (8d4dc84c), resent every kPlayResendMs while playing. The
//    audio board loads each preview itself when it lands (it owns the card),
//    named by its track and serial; the original it already holds, on the key, named by its
//    path's hash. Closing the studio releases the audio board's buffer. The playhead runs from
//    the press at the file's rate.
const u32 kPlayResendMs = 500;
// A22 The keys play the edit: a note on the track triggers the sample and hears it filtered.
//    While the studio is open, each new note on its track (stock's
//    per-track note serial 8d4d6c08, which keys and the sequencer both advance; read every frame)
//    plays the edit from its start through the player, as Shift + M3 does: at unity whatever the
//    key (an audition of the file), a new note restarting it. Releasing stops it like a gate,
//    so lifting a finger off the pad stops the sample: once no note of the track
//    is held in stock's note map (0x86bda258, track * 128 + note, which paints the keyboard;
//    note_activity.cpp), the player fades out (~6 ms). Shift + M3 still plays to the end. The track itself is silent meanwhile: the main board holds it with
//    B3 145 (track + 1, resent every kHoldResendMs; 0 on leave) and the audio board zeroes its
//    voices in the mixer (player.cpp P7), so the track's own notes still start and
//    end as stock runs them. On a drum or multisampler track every note plays the edit and the
//    whole track is silent while the studio is open.
const u32 kHoldResendMs = 250;
const u32 kTapMs = 100;   // a note never seen held within kTapMs: a tap, released
// A23 Play while rendering: an audition plays what has been rendered up to that point,
//    even if it grows in steps of a second or two. A full preview render (not
//    the quick first part) writes segment files of kSegmentFrames, each closed when full and never
//    opened again: t<track>-<serial>.a.wav, .b.wav, ... (a file reopened to append fails:
//    its seek doesn't take, it reopens at 0). After each, the frame tells the audio board
//    with B3 146 (serial | track << 24) and 147 (the frames on disk in all); its loader reads each
//    segment whole into its buffer at its place (player.cpp P8). Naming a segmented
//    preview in any command sends its count first. A key or Shift + M3 during such a render plays
//    the growing preview: what's loaded, stopping cleanly where it ends; a press later plays
//    further. When it lands, its last count goes out and the play goes on to the real end.
const u32 kSegmentFrames = 65536;   // ~1.5 s at 44.1 kHz: eight 32 KB writes of stereo
// A24 Live edits: an edit the live chain can play (live_ok: all but
//    beat-mode retime and print through FX) is never rendered for audition. A play sends its
//    settings (B3 148: index << 24 | value, 149: commit; only those that changed since the last)
//    and plays the original through the player's copy of the render chain (player.cpp
//    P9): the gain (level, or normalise over the markers as the render measures it), markers,
//    reverse, tape speed, the colour stage, DC, fades over the output's length and the channels.
//    Turning a knob while it plays sends the new set at once. The output's length for tape speed
//    is tape_frames(), the count the resampler makes. Beat-mode retime and print still render
//    (red bar, green flash, segments); every save renders.
// A25 The process page: rate, bits, drive, the filter sweep /
//    fx i, fx ii, tail, comp; no Shift rows on any page (trim has no silence, dc or chans: saves keep
//    the sample's own channels). fx i and fx ii are send amounts, heard live through whatever sits
//    in the FX I and FX II slots (B3 148 indices 12 and 13; player.cpp P10). Save
//    prints them, with `tail` seconds after the edit, by the real-time bounce (A29).
// A27 Save slices: on a drum sampler track a card offers "files" (the slice
//    files beside the original, as always) or "this kit" (the files, then on the kit's keys from the
//    selected one up, with undo; past the 24th key the rest are files only and the card says so).
//    The card shows on every track (A30); "this kit" only on a drum sampler.
// A30 New kit: the card's M2 saves the slices as a new drum kit preset, a
//    folder kPresetFolder/<stem>[-n].preset holding the slice files and a patch.json that puts
//    them on the kit's keys 53 up (lokey = hikey), one region a slice (kMaxSlices = 24, the kit's
//    size). Stock saves a user kit as /presets/snapshot/<date> (n).preset/patch.json (an empty
//    user kit as stock writes it: compact JSON, the folder holding only the patch): the patch is that
//    file's text with the regions filled in, its defaults as they are (volume 18348, the default
//    track volume). Each region copies stock's kit /content/presets/drum/zebra.json, the one stock
//    kit with regions to hand: its keys in its order, the sample named by its file in the
//    preset's folder (U: relative to the preset, as stock's kits name theirs from content/),
//    framecount 0 as zebra has it. Written last, on the worker, after every slice; a failure
//    removes the slices and the patch. Named after the sample, not the date. Stock's preset list
//    is assumed to rescan the folder when it opens. The studio stays on the original.
// A31 The highlighted file on a drum sampler or multisampler (Shift then Edit opens the
//    highlighted sample): there the library highlights without putting the file on the key, so
//    Shift + M1 opens the highlighted file when it isn't the key's own, for the selected key (studio.on_key):
//    the keys play it, save puts the result on the key (undo back to the key's own), slices can go on the kit.
// A32 Retime kept: with beat retime, the preview render holds only the retime (time_only);
//    level, normalise, colour, DC, fades and channels play live over it (the audio board's chain on the
//    loaded preview, mode 4, player.cpp P12), so turning them never renders again.
//    Saves still render the whole chain.
// A33 A pad plays its slice on the slice page: key 53 slice 1 and up (note_struck).
// A34 Neutral tuning (else kit slices play two octaves up): a file on an empty drum key gets root 60, and
//    slices on the kit root 60, fine 0 (stock_sample.hpp: kNeutralRootCents 6000).
// A35 The library lists saves at once, not only after a restart (New kit in presets > snapshot too).
//    Stock's library caches its listing and lists again only after a change clears its flag, as stock's own
//    preset save does. A save sets library_stale; closing the studio clears the flag and queues the library's
//    listing step, as entering the library does (relist_library). A save that ends after the close does it then.
// A36 A slice plays its own part of the source (with 24 slices and tape retime, pads 14 and up would be
//    silent): the cuts are output frames; the live chain plays source frames, so slice_source maps them back
//    as the waveform does (else a slice past the source end plays nothing, the earlier ones the wrong part).
// A44 A beat retime prints its sends (reverb included): the real-time print (A29) plays the
//    whole preview live (A32's kLiveFile) when it's on the card and loaded, under 20 s; the audio board's
//    capture waits for that play as for a live one (player.cpp P14). Longer: dry, the log says why.
// A45 Slices print too: the whole edit is printed live with its tail (A29, A44), then cut as A40 cuts its one
//    pass (start_print_split): the page's cuts on the edit's length, the tail on the last slice, so each slice
//    carries the ring-over of the one before, as a resample from a mixer would.
// A46 Reversed, the trim page mirrors (the waveform flips; the playhead doesn't run in reverse):
//    the source is drawn end first, the markers in playing order (the left one the edit's end), and the
//    playhead runs left to right (x_shown). The other pages already show the output in playing order.
// A47 Tail "full" renders until silence, capped in case of endless feedback: past 10 s the tail
//    reads "full": the print runs on after the edit until it stays under -60 dBFS for 2 s (keeping 0.1 s past
//    its last louder frame), at most 30 s; the file's header gives its length back. At the cap the card says
//    "tail cut at 30 s / still ringing". Offline (Lint) prints read it as 10 s.
// A39 The beat retime's search makes each probe once a hop (studio_dsp.hpp): the same
//    splices, bit for bit, at a fraction of the work. Previews and saves of a beat retime both run faster.
// A40 Slices of a retime in one pass (24 slices took very long otherwise): the length isn't known
//    before the chain ends, and counting it first would run the retime twice. So the chain writes one
//    temporary file at its own channels (render.in_path), then the slices are cut from it as it reads back:
//    the page's cuts on its real length, each slice's fades and channels (begin_split).
// A41 A save of a beat retime reuses its finished preview: when the whole
//    preview of this edit is on the card (rendered, segmented, under the 20 s limit) and no normalise asks for
//    the source's peak, the save reads the preview's segments in place of the original, with the retime off
//    (start_pipe): only the gain, colour, DC, fades and channels run. Longer edits retime (A40 for slices).
// A42 This kit onto a kit with fewer pads than slices: a pad is the first key whose
//    entry is that place; slices go on the pads from the selected one up, the rest stay files.
// A43 After this kit the studio stays on the source and its edit, as after New kit (A30).
// A38 A preview played while it rendered loads to its end: a key's
//    release sends its stop with kGrowing. Without it the audio board reads the stop as the preview landing, its
//    loader stops at what is on disk (65536 of 882000 frames) and the retime plays (A32, which wait for it whole)
//    never start, with no audio once fully rendered. The loader also carries on when a later count is larger (P13).
// A37 New kit in stock's preset index : closing after a new kit builds the index again, as stock's
//    preset rename and delete do (rescan_presets, 8d4eefec(0)).
const char kPresetFolders[] = "/fat32/presets", kPresetFolder[] = "/fat32/presets/snapshot";
// A29 Printing stock effects, whichever effects sit in the FX 1 and
//    FX 2 slots: a save with a send above 0 is bounced in real time, whatever effect is there: the studio plays the edit live through the real sends while the audio board captures its
//    dry block plus both send returns at the master bus point (player.cpp P11), the
//    edit's length plus the tail, then writes the file itself (it owns the card) as .studio/b<N>.wav.
//    The main board pokes it along (B3 152), waits for the file, renames it to the save's name and
//    finishes the save as any other. Other tracks' sends feed the same effects, so the card asks for
//    the sequencer stopped. Only a live edit bounces (beat retime prints dry, the card says why).
// A26 Compare and play latch, each turning to Stop: Shift + M2 plays the
//    original, Shift + M3 the edit; a second press stops, the other switches over, one plays at a
//    time, both through the player with the playhead. A key's note still plays only while held.
// A21 Retired: Lint printed offline on the main board. Every effect, Lint included,
//    prints by A29's real-time bounce, so the SDK carries no effect's code.
// A20 Output pages (slice, retime, process) draw one waveform: the render once it
//    exists, else the source mapped onto the output's estimated length. Its span stays
//    where it was last fitted, so a new length shows at once in proportion; when the
//    render lands the span eases to it (the waveform fits the width) under a green
//    flash (no second, overlapping waveform).
// A18 A time edit with no fade out (and not slices) renders in one pass, with no count
//    pass first: the header goes out with the estimated length, and once the chain
//    ends the file is opened again "r+b" and its header rewritten with the real one
//    (the data is the same frames the count would have measured). If that reopen or
//    write fails, or the cache later reads another length, every later render counts.
// A12 Stock's loader takes at most 882,000 frames (20 s at 44.1 kHz): a preview
//    stops there, and a key is never given more frames than that. A save writes its
//    whole edit, up to kSaveMaxFrames (so a x0.54 save of a 15.9 s source isn't cut
//    at 20 s); a longer save goes on its key as its first 20 s, and says so.
const u32 kMaxFrames = 882000;
const u32 kSaveMaxFrames = 4000000;   // ~90 s at 44.1 kHz; frame x 1024 waveform bins stays in 32 bits
// A47: tail 11 reads "full": the print runs on
// until it stays under -60 dBFS for 2 s after the edit (player.cpp P15), at most kFullTailS more.
const u32 kTailFull = 11, kFullTailS = 30;
u32 key_frames(u32 frames) { return frames > kMaxFrames ? kMaxFrames : frames; }

const u32 kSampler = host::engine_sampler, kDrumSampler = host::engine_drum, kMultisampler = host::engine_multisampler;
const u32 kEntryBytes = host::entry_bytes, kPadBytes = 32, kPathBytes = host::path_bytes;
const u32 kMaxEdits = 99;
const u32 kBins = 1024;
const u32 kUndoDepth = 12;
const u32 kMaxSlices = 24;
enum Page : u32 { kTrim, kChop, kTime, kColor };

// The device log takes 32-bit words: integers and pointers alike.
template <class T> i32 log_word(T value) { return static_cast<i32>(value); }
template <class T> i32 log_word(T* value) { return static_cast<i32>(reinterpret_cast<u32>(value)); }
template <class... A> void log(u32 line, const char* format, A... args) { host::log(line, format, log_word(args)...); }
// A14: a sample from stock's cache, wanted by stock or not. Worker only.
enum CacheState : u32 { kNotCached, kNotLoaded, kLoaded };
struct Borrowed { const void* object; CacheState state; bool unwanted; };
Borrowed borrow(u32 key) {
    const host::Borrowed h = host::borrow(key);
    const Borrowed b = {h.object, static_cast<CacheState>(h.state), h.unwanted};
    return b;
}
const char* cache_words(CacheState state) {
    return state == kNotCached ? "sample not in the cache" : state == kNotLoaded ? "sample not loaded yet" : "loaded";
}
void queue(void (*job)(u32), void (*done)(u32)) { host::post(job, done); }
unsigned char* region_entry(u32 track, u32 slot) { return host::key_entry(track, slot); }
void copy_bytes(unsigned char* to, const unsigned char* from, u32 count) {
    for (u32 i = 0; i < count; ++i) to[i] = from[i];
}
bool same_bytes(const unsigned char* a, const unsigned char* b, u32 count) {
    for (u32 i = 0; i < count; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
bool same_text(const char* a, const char* b, u32 limit) {
    for (u32 i = 0; i < limit; ++i) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}
void copy_text(char* to, const char* from, u32 size) {
    u32 i = 0;
    for (; i + 1 < size && from[i]; ++i) to[i] = from[i];
    to[i] = 0;
}
const char* base_name(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/') base = p + 1;
    return base;
}
bool guards_clear() { return !host::library_busy(); }
// A28 Edit from every sample browser. Stock has one
//    sample browser, the library, on every track (Shift + Sample, 8c09256a -> 8c094178(4)), and the
//    five hooks run in all of it. On a drum sampler or multisampler the selected key (8d4d6c98) is a
//    MIDI note, not a 0..23 slot (a test of slot < 24 never shows the label); stock's
//    region lookup 8d589370(track, note) folds it, so a key with a region is the test. The engine is
//    the SDK's sample-page engine (sdk_sample_ui_engine: an owned page-2 or page-7 machine counts as
//    the sampler or multisampler). Any other track opens the file the browser has highlighted, in
//    file mode: its full path from stock's tree (8c0a84b4(&string, 8c0ad5dc()), as stock's own press
//    reads it), its PCM from the cache, where the highlight has already put it (8c0a3054 -> desired
//    list at priority 99). There is no key, but the keys still play the edit with the track's own
//    engine held silent, as on a sample key; Save writes the file only.
bool selected(u32& track, u32& engine, u32& slot) {
    track = host::selected_track();
    if (track >= 8) return false;
    engine = host::sample_engine(track);
    if (engine != kSampler && engine != kDrumSampler && engine != kMultisampler) return false;
    slot = engine == kSampler ? 0 : host::selected_key(track);   // a note on drum and multi tracks
    return region_entry(track, slot) != nullptr;
}
// A35: stock's library caches its listing (the tree's flag at +0x6c) and lists a folder again only after a
// change clears it, as stock's own preset save does (8c0b4fce: 8c0a7e70(8c0ad5dc())). The studio's saves
// write files under the library, so once the studio has written and the library is shown again, it does the
// same, then queues the library's own listing step as entering the library does (8c0a2ee4: 8d4dd728(0, 0,
// 8c0a349c, 0)), which lists the folder again with the highlighted entry kept. Without it New kit and the
// saved files would appear only after a restart.
bool library_stale = false;
void relist_library() {
    library_stale = false;
    host::relist_library();
    log(__LINE__, "library: listed again after the studio's save");
}
// A37 (a new kit in presets > snapshot): stock's preset browser reads an index of every
// preset that stock builds at boot ("scan presets": 8d4eefec(1), 8c0b8d30), not the folder, and the library's
// listing (A35) is apart from it. Stock's own preset rename and delete build it again with 8d4eefec(0) on the UI
// thread (8d4ef74c, 8d4ef670); the studio does the same as soon as a new kit is written. It reads the card:
// about 1.4 s at boot, so the screen holds that long, once, after a new kit.
void rescan_presets() {
    const u32 t0 = now_ms();
    host::rescan_presets();
    log(__LINE__, "presets: stock's preset list built again for the new kit, %d ms", static_cast<int>(now_ms() - t0));
}
// A35: a save wrote files. The studio open: the library lists them again when it closes; closed (a render
// that ends after the studio closed): now.
bool studio_closed();
void files_written() {
    library_stale = true;
    if (studio_closed()) relist_library();
}
// A28: a cheap proxy for the label: the last highlighted file stock keeps (0x84039408, 96 bytes).
i32 frames_of(const unsigned char* entry) {
    return host::entry_frames(entry);
}
u32 files_set_ms;
bool files_pending;
bool waveform_said;   // the first waveform of this opening is logged once it's drawn
// The output pages' span and the finish flash (A20), and the header's slide.
u32 axis_frames, axis_from, axis_to, axis_t0;
bool axis_moving;
u32 flash_t0;
bool flashing;
opxy::draw::PageHeader header_state;
u32 header_ms;
void landed(u32 frames);
void remove_passed();
void load_preview();
void growing_landed(u32 serial, u32 length);
void send_grow(u32 serial, u32 frames);
void release_player();
void send_hold(u32 value);
bool live_sent_valid;     // A24: the audio board holds the set last sent
bool slice_choice_made;   // A27: the card has been answered for this save
bool slices_to_kit;       // A27: "this kit"
bool slices_new_kit;      // A30: "new kit"
bool sends_valid;         // A25: and the send amounts last sent
u32 sends_sent[2];
void send_sends();
u32 track_notes();
u32 note_seen;            // A22: the track's note serial last seen
void files_changed() {   // A15
    host::samples_changed();
    files_set_ms = now_ms();
    files_pending = true;
}
void send_regions(u32 track, bool files = false) {
    host::send_regions(track, false);
    if (files) files_changed();
}
// Put a file on a key as the library does (A6), without sending the table.
bool assign_file(u32 track, u32 engine, u32 slot, const char* path, u32 frames, u32 root) {
    // A34: an empty drum key's root and fine (0, 0) read as 60 semitones up; the SDK gives it 60, 0
    return host::put_file(track, engine, slot, path, frames, root);
}

// ------------------------------------------------------------------ the edit
struct Edit {
    // trim
    u32 start, end;          // source frames, start < end
    u32 fade_in, fade_out;   // ms
    i32 level;               // dB, -30..+20
    u32 normal;              // 0 off, 1..4: 0, -1, -3, -6 dBFS
    u32 reverse, snap;       // 0 or 1
    u32 silence;             // 0 off, 1..7: -60..-24 dB (the last trim applied)
    u32 dc;                  // 0 or 1
    u32 chans;               // Channels
    // chop
    u32 slices, chop_mode, sens, slice, chop_fade, chop_snap;   // mode 0 even, 1 transient; slice 0 all
    // time
    u32 stretch;             // percent, 25..400
    i32 pitch;               // semitones (beat), -24..24
    u32 beat, fit, grain;    // fit 0 off, 1..4: 1, 2, 4, 8 bars
    // process (A25: rate bits drive filter / fx i fx ii tail comp)
    u32 rate_index, bits_index, drive;
    i32 sweep;               // the filter: -48..-1 low-pass, 0 off, 1..48 high-pass
    u32 comp;                // 0..100
    // the sends (A25): heard live through FX I and FX II; Save prints them (A29)
    u32 send1, send2, tail;  // 0..100 each; tail seconds 0..10
};
bool same_edit(const Edit& a, const Edit& b) {
    return same_bytes(reinterpret_cast<const unsigned char*>(&a), reinterpret_cast<const unsigned char*>(&b), sizeof(Edit));
}
void neutral(Edit& e) {
    e.fade_in = e.fade_out = 0;
    e.normal = e.silence = e.dc = e.chans = 0;
    e.snap = 1;              // markers snap to zero crossings
    e.slices = 8;
    e.chop_mode = 1;         // slice starts at the transients; even is a turn away
    e.sens = 50;
    e.slice = 0;
    e.chop_fade = 2;
    e.chop_snap = 0;
    e.stretch = 100;
    e.pitch = 0;
    e.beat = 1;
    e.fit = 0;
    e.grain = 40;
    e.rate_index = e.bits_index = e.drive = 0;
    e.sweep = 0;
    e.comp = 0;
    e.send1 = e.send2 = 0;
    e.tail = 2;
}
dsp::Settings settings_of(const Edit& e) {
    dsp::Settings s = {};
    s.begin = e.start;
    s.end = e.end;
    s.reverse = e.reverse;
    s.gain = kUnity;
    s.dc = e.dc;
    s.stretch = e.stretch;
    s.pitch = e.pitch;
    s.beat = e.beat;
    s.grain_ms = e.grain;
    s.rate_index = e.rate_index;
    s.bits_index = e.bits_index;
    s.drive = e.drive;
    s.sweep = e.sweep;
    s.comp = e.comp;
    return s;
}
// What the original between the markers at the key's level isn't: everything but markers, reverse and level.
bool needs_render(const Edit& e) {
    const dsp::Settings s = settings_of(e);
    return e.fade_in || e.fade_out || e.dc || e.chans || dsp::time_active(s) || dsp::color_active(s) || e.send1 || e.send2;
}
// A24: what the player's live chain plays: everything but beat-mode retime and print.
bool live_ok(const Edit& e) { return !(e.beat && dsp::time_active(settings_of(e))); }   // A25: sends are live too
bool print_dry;   // A29: the last save rendered a send dry (a beat retime can't bounce)
bool print_capped;   // A47: the last print's "full" tail ran to the cap
u32 dry_engine;   // A29: the effect that couldn't (stock's fx ids, op1_factory.json)
const char* effect_name(u32 id) {
    switch (id) {
    case 0: return "delay";
    case 5: return "reverb";
    case 12: return "chorus";
    case 13: return "phaser";
    case 14: return "dist";
    case 15: return "lofi";
    default: return "the effect";
    }
}
// A24: an edit whose audition still needs a rendered preview.
bool preview_render(const Edit& e) { return needs_render(e) && !live_ok(e); }
// A32: changing any other value (e.g. drive) applies it to the timestretched render rather than
// rendering the audio again. With beat retime the preview holds only what the live chain
// can't do, the source between the markers, reversed, retimed; the gain, colour, DC, fades and channels
// play live over it (preview_live). These are the fields that render hears.
bool same_time(const Edit& a, const Edit& b) {
    return a.start == b.start && a.end == b.end && a.reverse == b.reverse && a.stretch == b.stretch &&
           a.pitch == b.pitch && a.beat == b.beat && a.grain == b.grain;
}
Edit time_only(const Edit& e) {
    Edit t = e;
    t.level = 0;
    t.normal = 0;
    t.dc = 0;
    t.fade_in = t.fade_out = 0;
    t.chans = 0;
    t.rate_index = t.bits_index = 0;
    t.drive = 0;
    t.sweep = 0;
    t.comp = 0;
    t.send1 = t.send2 = 0;
    return t;
}
// A24: the frames the chain makes from `length` source frames at a tape stretch: the resampler
// yields frame k while floor(k speed / 2^16) + 2 <= length, speed = floor(6553600 / stretch),
// so ceil((length - 1) 2^16 / speed), in 32-bit parts (length - 1 < 2^20, speed < 2^19).
u32 tape_frames(u32 length, u32 stretch) {
    if (stretch == 100 || stretch == 0) return length;
    const u32 speed = 6553600u / stretch;
    if (length < 2) return 0;
    const u32 l = length - 1;
    const u32 q1 = l / speed, r1 = l % speed;
    const u32 q2 = (r1 << 8) / speed, r2 = (r1 << 8) % speed;
    const u32 q3 = (r2 << 8) / speed, r3 = (r2 << 8) % speed;
    return (q1 << 16) + (q2 << 8) + q3 + (r3 ? 1u : 0u);
}

enum Mode : u32 { kClosed, kOpen, kSaving, kLeaving, kSliceChoice };   // kSliceChoice: A27's card
enum UndoKind : u32 { kEditStep, kFileStep, kKitStep };
struct UndoRecord {
    UndoKind kind;
    Edit edit, opened;
    char path[kPathBytes];
    unsigned char entry[kEntryBytes];  // the key's entry before the save loaded its file
    unsigned char origin[kEntryBytes];
};
struct Studio {
    Mode mode;
    Page page;
    u32 track, engine, slot;
    char path[kPathBytes];             // the file being edited (never a preview)
    Edit edit, opened;
    unsigned char origin[kEntryBytes]; // the key's entry as the studio opened on this file
    u32 row;
    bool compare, leave_after_save;
    bool save_pending;                 // a save asked for while a preview rendered: runs once it stops
    // the PCM (A9)
    u32 key;
    const void* object;
    opxy::SampleView view;
    bool loading, ready, pcm_failed;
    bool file_only;                    // A28: a file from the browser, on no key
    i32 file_root;                     // A49: its own root in cents from note 0 (6000: middle C, none in the file)
    bool on_key;                       // A31: a file from the browser, for the selected key (drum, multi)
    unsigned char key_entry[kEntryBytes];  // A31: that key's entry as the studio opened
    u32 retry_ms;
    unsigned short env[kBins];
    u32 peak;
    // live preview (A4)
    bool live_dirty;
    u32 flushed_ms;
    // the rendered preview (A11)
    u32 change;                        // bumped by every change a render would hear
    u32 changed_ms;
    u32 rendered;                      // the change the current preview file holds; 0 none
    bool showing_preview;              // the key plays the preview file
    char preview[64];
    u32 preview_frames, preview_channels;
    u32 serial_first;                  // this session's oldest preview file not yet removed
    u32 shown_serial;                  // the preview file on the key (A13)
    bool loading_preview, checking, resent, preview_lost;
    u32 swapped_ms, next_check_ms;
    u32 failed_change, failures, retry_at_ms;
    u32 quick_change;                  // the change whose first kQuickMs are on the key (A17)
    Edit time_last;                    // A32: the edit at the last touch, for its retime fields
    bool time_valid;
    bool preview_streamed;             // the preview on the key was rendered in one pass (A18)
    bool shown_segmented;              // A23: the shown preview is segment files
    unsigned short out_env[kBins];     // the last render's waveform
    u32 out_frames;
    bool out_valid;
    bool cut_at_limit;
    // the view
    u32 zoom_marker, zoom_until;
    i32 gesture;
    u32 turn_ms, run;
    // the card
    u32 card_until;
    char card1[24], card2[24];
    UndoRecord undo[kUndoDepth];
    u32 undo_count;
    // chop
    u32 cuts[kMaxSlices + 1];          // output frames, cuts[0] = 0, cuts[count] = length
    u32 cut_q16[kMaxSlices + 1];       // the same as fractions of the length, Q16
    u32 cut_count;                     // slices
    // the library
    u32 library_seen_ms;
    bool library_visible;
    u32 generation;
} studio;
bool studio_closed() { return studio.mode == kClosed; }
// A drum kit loaded from slices: the keys as they were (one level of undo).
struct KitUndo {
    bool valid;
    u32 track, first, count;
    u32 notes[kMaxSlices];             // A28: each key by the note stock folds onto it
    unsigned char entries[kMaxSlices][kEntryBytes];
} kit_undo;

struct Pending {
    bool valid;
    u32 track, slot;
    char path[kPathBytes];
    Edit edit, opened;
    unsigned char origin[kEntryBytes];
} pending;

struct Snapshot {
    bool valid;
    u32 track, slot;
    char path[kPathBytes];
    unsigned char entry[kEntryBytes];
} snapshot;

// --------------------------------------------------------------- rendering
// One render at a time, on the file worker: to the preview file, to a saved
// edit, or to chop slices. Two passes when the length isn't known (time stage):
// a count, then the write.
enum Target : u32 { kToPreview, kToSave, kToSlices };
struct Render {
    Target target;
    u32 change;                        // the studio change it renders
    u32 track, engine, slot, key, root;
    Edit edit;
    u32 normal;
    i32 level;
    u32 chans, out_channels, channels, rate, gain, peak;
    u32 length, done, written, writes, fi, fo, step_in, step_out;
    bool counting, touched;
    dsp::Pipeline pipe;
    const void* object;
    File* file;
    unsigned char* chunk;
    u32 fill;
    Dither dither;
    bool failed;
    const char* why;
    char source[kPathBytes];
    char final_path[112];
    char temporary[112];
    u32 serial, started_ms;
    bool quick, superseded;
    bool segmented;                    // A23: a full preview, written as segment files
    u32 synced, segments;              // A23: frames in closed segments, and how many are closed
    bool streaming;                    // A18: one pass, the header rewritten at the end
    bool two_pass;                     // a count pass, then the write (progress: half each)
    u32 estimate;
    // slices
    u32 cuts[kMaxSlices + 1], cut_count, slice, slice_done;
    char slice_paths[kMaxSlices][104];
    bool new_kit;                      // A30
    char kit_folder[kPathBytes];
    u32 slice_frames[kMaxSlices];
    // A40, A41: frames read back from the card, a 32 KB read at a time
    bool from_preview;                 // A41: the source is the finished preview's segments, the retime done
    bool printed;                      // A45: the slices are cut from a real-time print (FX I, FX II, the tail)
    bool split, splitting;             // A40: slices of a retime in one pass: a temporary render, then cut
    u32 split_chans, split_out;        // A40: the slices' channels, held while the one pass writes the chain's
    File* in_file;
    opxy::i16* in_buf;
    u32 in_start, in_frames, in_file_end, in_total, in_channels;
    bool in_segmented;
    char in_path[112];                 // the temporary render, or the preview (its segments by its name)
    // the output's waveform, for the stage
    unsigned short env[kBins];
} render;
volatile u32 busy;
bool header_patch_failed;              // A18 off for the rest of this boot
volatile bool grow_due;                // A23: the worker closed a segment; the frame tells the audio board
unsigned char header[kHeaderBytes];

// A12: what a render may write.
u32 frame_limit() { return render.target == kToPreview ? kMaxFrames : kSaveMaxFrames; }
void fail(const char* why) {
    render.failed = true;
    render.why = why;
}
bool choose_paths() {
    char stem[kPathBytes];
    path_stem(render.source, stem, sizeof stem);
    char beside[kPathBytes];
    path_folder(render.source, beside, sizeof beside);
    const char* folders[2] = {beside, kUserFolder};
    for (u32 c = starts_with(render.source, "/fat32/") ? 0 : 1; c < 2; ++c) {
        for (u32 number = 1; number <= kMaxEdits; ++number) {
            if (!edit_path(render.final_path, sizeof render.final_path, folders[c], stem, number, ".wav") ||
                !edit_path(render.temporary, sizeof render.temporary, folders[c], stem, number, ".tmp"))
                break;
            if (file_exists(render.final_path)) continue;
            File* probe = file_open(render.temporary, "wb");
            if (!probe) break;
            file_close(probe);
            return true;
        }
    }
    return false;
}
// <folder>/<stem>-s<k>.wav, or -<round>s<k> when an earlier chop holds the names.
bool slice_path(char* out, u32 size, const char* folder, const char* stem, u32 round, u32 k) {
    u32 n = 0;
    out[0] = 0;
    if (!append(out, size, n, folder) || !append(out, size, n, "/") || !append(out, size, n, stem) ||
        !append(out, size, n, "-"))
        return false;
    if (round > 1 && (!append_number(out, size, n, round))) return false;
    return append(out, size, n, "s") && append_number(out, size, n, k) && append(out, size, n, ".wav") && n <= 95;
}
// A30: kPresetFolder/<stem>.preset, or <stem>-<n>.preset when that one holds a patch or the names.
bool kit_file(char* out, u32 size, const char* name) {
    u32 n = 0;
    out[0] = 0;
    return append(out, size, n, render.kit_folder) && append(out, size, n, "/") && append(out, size, n, name);
}
bool choose_kit_paths(const char* stem) {
    make_folder(kPresetFolders);
    make_folder(kPresetFolder);
    for (u32 round = 1; round <= 20; ++round) {
        u32 n = 0;
        render.kit_folder[0] = 0;
        if (!append(render.kit_folder, sizeof render.kit_folder, n, kPresetFolder) ||
            !append(render.kit_folder, sizeof render.kit_folder, n, "/") ||
            !append(render.kit_folder, sizeof render.kit_folder, n, stem[0] ? stem : "kit"))
            return false;
        if (round > 1 && (!append(render.kit_folder, sizeof render.kit_folder, n, "-") ||
                          !append_number(render.kit_folder, sizeof render.kit_folder, n, round)))
            return false;
        if (!append(render.kit_folder, sizeof render.kit_folder, n, ".preset")) return false;
        char patch[kPathBytes + 16];
        if (!kit_file(patch, sizeof patch, "patch.json")) return false;
        if (file_exists(patch)) continue;
        make_folder(render.kit_folder);
        bool free = true;
        for (u32 k = 0; k < render.cut_count && free; ++k) {
            if (!slice_path(render.slice_paths[k], sizeof render.slice_paths[k], render.kit_folder, stem, 1, k + 1))
                return false;
            if (file_exists(render.slice_paths[k])) free = false;
        }
        if (!free) continue;
        File* probe = file_open(render.slice_paths[0], "wb");
        if (!probe) return false;
        file_close(probe);
        remove_file(render.slice_paths[0]);
        return true;
    }
    return false;
}
// A30: the patch.json, the empty kit's fields (stock's writer, compact) around one region a slice.
const char kKitHead[] =
    "{\"engine\":{\"bendrange\":8191,\"detune\":0,\"highpass\":0,\"modulation\":{\"aftertouch\":{\"amount\":1"
    "6383,\"target\":0},\"modwheel\":{\"amount\":16383,\"target\":0},\"pitchbend\":{\"amount\":16383,\"target"
    "\":0},\"velocity\":{\"amount\":16383,\"target\":0}},\"params\":[16384,16384,16384,16384,16384,16384,1638"
    "4,16384],\"playmode\":\"poly\",\"portamento.amount\":0,\"portamento.type\":32767,\"transpose\":0,\"tunin"
    "g.root\":0,\"tuning.scale\":0,\"velocity.sensitivity\":19660,\"volume\":18348,\"width\":0},\"envelope\":"
    "{\"amp\":{\"attack\":0,\"decay\":0,\"release\":1000,\"sustain\":32767},\"filter\":{\"attack\":0,\"decay\""
    ":3276,\"release\":23757,\"sustain\":983}},\"fx\":{\"active\":false,\"params\":[22014,0,30285,11880,0,327"
    "67,0,0],\"type\":\"ladder\"},\"lfo\":{\"active\":false,\"params\":[20309,5679,19114,15807,0,0,0,12287],\""
    "type\":\"random\"},\"octave\":0,\"platform\":\"OP-XY\",\"regions\":[";
const char kKitRegionA[] = "{\"fade.in\":0,\"fade.out\":0,\"framecount\":0,\"hikey\":";
const char kKitRegionB[] = ",\"lokey\":";
const char kKitRegionC[] = ",\"pan\":0,\"pitch.keycenter\":60,\"playmode\":\"oneshot\",\"reverse\":false,\"sample\":\"";
const char kKitRegionD[] = "\",\"transpose\":0,\"tune\":0}";
const char kKitTail[] = "],\"type\":\"drum\",\"version\":4}\n";
const u32 kKitFirstKey = 53;
// JSON text for a file name: quotes and backslashes escaped, control characters dropped.
bool append_json(char* out, u32 size, u32& n, const char* text) {
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20) continue;
        if ((c == '"' || c == '\\') && !append(out, size, n, "\\")) return false;
        const char one[2] = {static_cast<char>(c), 0};
        if (!append(out, size, n, one)) return false;
    }
    return true;
}
// Worker (render_last): false leaves no patch behind.
bool write_kit_patch() {
    char path[kPathBytes + 16];
    if (!kit_file(path, sizeof path, "patch.json")) return false;
    File* f = file_open(path, "wb");
    if (!f) return false;
    bool ok = file_write(kKitHead, 1, sizeof kKitHead - 1, f) == sizeof kKitHead - 1;
    for (u32 k = 0; ok && k < render.cut_count && k < kMaxSlices; ++k) {
        char region[256];
        u32 n = 0;
        region[0] = 0;
        ok = (!k || append(region, sizeof region, n, ",")) && append(region, sizeof region, n, kKitRegionA) &&
             append_number(region, sizeof region, n, kKitFirstKey + k) && append(region, sizeof region, n, kKitRegionB) &&
             append_number(region, sizeof region, n, kKitFirstKey + k) && append(region, sizeof region, n, kKitRegionC) &&
             append_json(region, sizeof region, n, base_name(render.slice_paths[k])) &&
             append(region, sizeof region, n, kKitRegionD);
        if (ok) ok = file_write(region, 1, n, f) == n;
    }
    if (ok) ok = file_write(kKitTail, 1, sizeof kKitTail - 1, f) == sizeof kKitTail - 1;
    file_close(f);
    if (!ok) remove_file(path);
    return ok;
}
bool choose_slice_paths() {
    char stem[kPathBytes];
    path_stem(render.source, stem, sizeof stem);
    if (stem[0] && stem[16]) stem[16] = 0;   // keep the names short enough for every slice
    char beside[kPathBytes];
    path_folder(render.source, beside, sizeof beside);
    if (render.new_kit) return choose_kit_paths(stem);
    const char* folders[2] = {beside, kUserFolder};
    for (u32 c = starts_with(render.source, "/fat32/") ? 0 : 1; c < 2; ++c) {
        for (u32 round = 1; round <= 20; ++round) {
            bool free = true;
            for (u32 k = 0; k < render.cut_count && free; ++k) {
                if (!slice_path(render.slice_paths[k], sizeof render.slice_paths[k], folders[c], stem, round, k + 1))
                    return false;
                if (file_exists(render.slice_paths[k])) free = false;
            }
            if (!free) continue;
            File* probe = file_open(render.slice_paths[0], "wb");
            if (!probe) break;
            file_close(probe);
            remove_file(render.slice_paths[0]);
            return true;
        }
    }
    return false;
}
void scratch_path(char* out, u32 size, u32 track, u32 number) {
    u32 n = 0;
    out[0] = 0;
    append(out, size, n, kScratchFolder);
    append(out, size, n, "/t");
    append_number(out, size, n, track + 1);
    append(out, size, n, "-");
    append_number(out, size, n, number);
    append(out, size, n, ".wav");
}

// A23: segment k of a preview: "t3-5.wav" -> "t3-5.a.wav".
void segment_path(char* out, u32 size, const char* preview, u32 k) {
    u32 n = 0, length = 0;
    while (preview[length]) ++length;
    out[0] = 0;
    for (; n + 1 < size && n + 4 < length; ++n) out[n] = preview[n];
    out[n] = 0;
    const char letter[3] = {'.', static_cast<char>('a' + k), 0};
    append(out, size, n, letter);
    append(out, size, n, ".wav");
}
// Fades and channels, then 16 bits: one output frame into the chunk.
void put_frame(u32 i, u32 n, const i32* w) {
    u32 g = kUnity;
    if (i < render.fi) g = i * render.step_in;
    const u32 back = n - 1 - i;
    if (back < render.fo && back * render.step_out < g) g = back * render.step_out;
    i32 v[2] = {w[0], w[1]};
    if (g < kUnity)
        for (u32 c = 0; c < render.channels; ++c) v[c] = fade_word(v[c], g);
    i32 out[2] = {v[0], v[1]};
    if (render.chans == kMono) out[0] = mono_word(v[0], v[1]);
    else if (render.chans == kRight) out[0] = v[1];
    for (u32 c = 0; c < render.out_channels; ++c) {
        const i32 s = render.touched ? render.dither.sample(out[c]) : out[c] >> 16;
        put16(render.chunk + render.fill, static_cast<u32>(s) & 0xffffu);
        render.fill += 2;
    }
}
bool flush_chunk() {
    if (!render.fill) return true;
    const u32 wrote = file_write(render.chunk, 1, render.fill, render.file);
    render.written += wrote;
    ++render.writes;
    const bool ok = wrote == render.fill;
    render.fill = 0;
    if (!ok) fail("storage full?");
    return ok;
}
// A23: the frames a segment's header gives: a whole segment, or what's left of a known length.
u32 segment_frames(u32 k) {
    if (render.streaming) return kSegmentFrames;   // the last one is shorter; the count sent is the truth
    const u32 left = render.length - k * kSegmentFrames;
    return left < kSegmentFrames ? left : kSegmentFrames;
}
bool open_output(const char* path, u32 frames);
// A23: a full segment of a preview is closed (complete on the card) and the next opened.
bool next_segment() {
    if (!render.segmented || !render.file) return true;
    if (render.done - render.synced < kSegmentFrames || render.done >= render.length) return true;
    file_close(render.file);
    render.file = nullptr;
    render.synced = render.done;
    if (!render.segments++)
        log(__LINE__, "preview %s: first %d frames on the card after %d ms", render.final_path,
            static_cast<int>(render.done), static_cast<int>(now_ms() - render.started_ms));
    grow_due = true;   // B3 goes out from the GUI task, as every other studio message
    char path[112];
    segment_path(path, sizeof path, render.final_path, render.segments);
    return open_output(path, segment_frames(render.segments));
}
bool open_output(const char* path, u32 frames) {
    render.file = file_open(path, "wb");
    if (!render.file) {
        fail("can't open the file");
        return false;
    }
    wav_header(header, frames, render.out_channels, render.rate);
    if (file_write(header, 1, kHeaderBytes, render.file) != kHeaderBytes) {
        fail("storage full?");
        return false;
    }
    return true;
}
// A40, A41: the frames from `pos` in render.in_buf, read in order: each file opens at its start
// (a preview's segment k at k * kSegmentFrames), past its 44-byte header, and reads stop at its end.
const u32 kInFrames = kChunkFrames;
bool in_open(u32 pos) {
    if (render.in_file) file_close(render.in_file);
    render.in_file = nullptr;
    char path[112];
    u32 first = 0;
    if (render.in_segmented) {
        const u32 k = pos / kSegmentFrames;
        segment_path(path, sizeof path, render.in_path, k);
        first = k * kSegmentFrames;
        render.in_file_end = render.in_total - first > kSegmentFrames ? first + kSegmentFrames : render.in_total;
    } else {
        copy_text(path, render.in_path, sizeof path);
        render.in_file_end = render.in_total;
    }
    if (pos != first) { fail("couldn't read back"); return false; }
    render.in_file = file_open(path, "rb");
    unsigned char skip[kHeaderBytes];
    if (!render.in_file || file_read(skip, 1, kHeaderBytes, render.in_file) != kHeaderBytes) {
        fail("couldn't read back");
        return false;
    }
    return true;
}
bool in_ready(u32 pos) {
    if (pos >= render.in_start && pos < render.in_start + render.in_frames) return true;
    if (pos != render.in_start + render.in_frames || pos >= render.in_total) { fail("couldn't read back"); return false; }
    if (!render.in_file || pos >= render.in_file_end) {
        if (!in_open(pos)) return false;
    }
    u32 n = render.in_file_end - pos;
    if (n > kInFrames) n = kInFrames;
    const u32 bytes = n * render.in_channels * 2;
    if (file_read(render.in_buf, 1, bytes, render.in_file) != bytes) { fail("couldn't read back"); return false; }
    render.in_start = pos;
    render.in_frames = n;
    return true;
}
// Worker: the first job. Borrow the PCM, set up the chain, start counting or writing.
void start_pipe() {
    if (render.from_preview) {
        // A41: the preview already holds the markers, reverse and retime; the rest runs as for the original
        Edit t = render.edit;
        t.start = 0;
        t.end = render.in_total;
        t.reverse = 0;
        t.stretch = 100;
        t.pitch = 0;
        t.beat = 0;
        dsp::Settings s = settings_of(t);
        s.gain = render.gain;
        render.pipe.start(render.in_buf, render.in_channels, render.rate, s);   // data follows the buffer
        return;
    }
    const opxy::SampleView view = host::sample_view(render.object);
    dsp::Settings s = settings_of(render.edit);
    s.gain = render.gain;
    render.pipe.start(view.data, view.channels, view.rate, s);
}
void begin_write();
void render_first(u32) {
    if (render.from_preview) {
        // A41: no original to borrow: the preview's frames, at the original's rate
        render.channels = render.in_channels;
        if (render.channels < 2) render.chans = kKeep;
        render.out_channels = render.chans == kKeep ? render.channels : 1;
        render.gain = level_gain(render.level);
        render.in_buf = static_cast<opxy::i16*>(allocate(kInFrames * render.in_channels * 2));
        if (!render.in_buf) { fail("no memory"); return; }
        render.counting = false;
        render.length = render.in_total;
        render.streaming = false;
        render.two_pass = false;
        render.done = 0;
        start_pipe();
        begin_write();
        return;
    }
    const Borrowed b = borrow(render.key);
    render.object = b.object;
    if (!render.object) { fail(cache_words(b.state)); return; }
    const opxy::SampleView view = host::sample_view(render.object);
    if (!view.data || !view.frames) { fail("no sample"); return; }
    Edit& e = render.edit;
    if (e.end > view.frames) e.end = view.frames;
    if (e.start >= e.end) { fail("nothing to save"); return; }
    render.channels = view.channels;
    render.rate = view.rate;
    if (render.channels < 2) render.chans = kKeep;
    render.out_channels = render.chans == kKeep ? render.channels : 1;
    if (render.normal) {
        render.peak = 0;
        for (u32 f = e.start; f < e.end; ++f)
            for (u32 c = 0; c < render.channels; ++c) {
                const i32 v = view.data[f * render.channels + c];
                const u32 m = v < 0 ? static_cast<u32>(-v) : static_cast<u32>(v);
                if (m > render.peak) render.peak = m;
            }
        render.gain = normalise_gain(render.peak, kNormalTarget[render.normal - 1]);
    } else {
        render.gain = level_gain(render.level);
    }
    if (render.target == kToPreview) make_folder(kScratchFolder);
    const dsp::Settings s = settings_of(e);
    render.counting = dsp::time_active(s);
    render.length = render.counting ? 0 : e.end - e.start;
    if (render.quick) {
        // the first kQuickMs only (A17): no count pass, silence if the chain ends sooner
        const u32 quick = render.rate * kQuickMs / 1000u;
        if (render.counting || render.length > quick) render.length = quick;
        render.counting = false;
    }
    render.streaming = false;
    render.split = render.target == kToSlices && render.counting;   // A40
    if (render.split) {
        // A40: the chain's own channels into the temporary file; the slices take theirs when cut
        make_folder(kScratchFolder);
        u32 n = 0;
        render.in_path[0] = 0;
        append(render.in_path, sizeof render.in_path, n, kScratchFolder);
        append(render.in_path, sizeof render.in_path, n, "/split");
        append_number(render.in_path, sizeof render.in_path, n, render.track + 1);
        append(render.in_path, sizeof render.in_path, n, ".wav");
        render.split_chans = render.chans;
        render.split_out = render.out_channels;
        render.chans = kKeep;
        render.out_channels = render.channels;
    }
    if (render.counting && (render.split || (!e.fade_out && render.target != kToSlices && !header_patch_failed))) {
        // A18: one pass; the write stops where the chain ends (A40: the slices' too, cut afterwards)
        render.streaming = true;
        render.counting = false;
        const u32 estimate = (e.end - e.start) * e.stretch / 100u;
        render.estimate = estimate < 1 ? 1 : estimate > frame_limit() ? frame_limit() : estimate;
        render.length = frame_limit();
    }
    render.two_pass = render.counting;
    render.done = 0;
    start_pipe();
    if (!render.counting) begin_write();
}
// Worker: once the length is known, the paths, fades and the first file.
void begin_slices(u32 n, u32 end);
void begin_write() {
    if (render.length > frame_limit()) render.length = frame_limit();
    if (!render.length) { fail("nothing to save"); return; }
    const Edit& e = render.edit;
    const u32 n = render.length;
    render.fi = e.fade_in ? render.rate * e.fade_in / 1000u : 0;
    render.fo = e.fade_out ? render.rate * e.fade_out / 1000u : 0;
    if (render.quick) render.fo = render.rate * kQuickCutMs / 1000u;   // the cut, not the edit's fade out
    if (render.split) render.fi = render.fo = 0;   // A40: each slice's own fades, when cut
    if (render.fi > n) render.fi = n;
    if (render.fo > n) render.fo = n;
    render.step_in = render.fi ? kUnity / render.fi : 0;
    render.step_out = render.fo ? kUnity / render.fo : 0;
    const dsp::Settings s = settings_of(e);
    render.touched = render.gain != kUnity || e.dc || render.fi || render.fo || render.chans == kMono ||
                     dsp::time_active(s) || dsp::color_active(s) || (render.target == kToSlices && e.chop_fade);
    render.chunk = static_cast<unsigned char*>(allocate(kChunkFrames * render.out_channels * 2));
    if (!render.chunk) { fail("no memory"); return; }
    render.fill = 0;
    render.done = 0;
    for (u32 b = 0; b < kBins; ++b) render.env[b] = 0;
    if (render.counting) start_pipe();
    render.counting = false;
    if (render.target == kToSave) {
        if (!choose_paths()) { fail("no writable folder"); return; }
        open_output(render.temporary, render.streaming ? render.estimate : n);
    } else if (render.target == kToPreview && !render.quick) {
        // A23: segment files
        render.segmented = true;
        char path[112];
        segment_path(path, sizeof path, render.final_path, 0);
        open_output(path, segment_frames(0));
    } else if (render.target == kToPreview) {
        open_output(render.final_path, render.streaming ? render.estimate : n);
    } else if (render.split) {
        // A40: one pass into the temporary file; the slices are cut from it once its length is known
        open_output(render.in_path, render.estimate);
    } else {
        begin_slices(n, n);
    }
}
// The slices: the page's cuts, as fractions, on the output's real length; the first one opened. The last
// runs to `end` (A45: a print's tail after the edit).
void begin_slices(u32 n, u32 end) {
    render.cut_count = studio.cut_count;
    for (u32 k = 0; k <= render.cut_count; ++k) {
        // n * q16 / 2^16 in 32 bits, exact for outputs past 2^20 frames (saves up to kSaveMaxFrames)
        const u32 q16 = studio.cut_q16[k];
        const u32 at = (n >> 16) * q16 + (((n & 0xffffu) * q16) >> 16);
        render.cuts[k] = at > n ? n : at;
        if (k && render.cuts[k] <= render.cuts[k - 1]) render.cuts[k] = render.cuts[k - 1] + 1;
    }
    render.cuts[0] = 0;
    render.cuts[render.cut_count] = end > render.cuts[render.cut_count - 1] ? end : render.cuts[render.cut_count - 1] + 1;
    if (!choose_slice_paths()) { fail("no writable folder"); return; }
    render.slice = 0;
    render.slice_done = 0;
    const u32 frames = render.cuts[1] - render.cuts[0];
    render.slice_frames[0] = frames;
    open_output(render.slice_paths[0], frames);
}
// One output frame: the stage's waveform, then the chunk (written out when full or done).
bool write_out(const i32* w) {
    const u32 i = render.done;
    u32 bin = i * kBins / (render.streaming ? render.estimate : render.length);
    if (bin >= kBins) bin = kBins - 1;
    for (u32 c = 0; c < render.channels; ++c) {
        const i32 a = w[c] >> 16;
        const u32 m = static_cast<u32>(a < 0 ? -a : a);
        if (m > render.env[bin]) render.env[bin] = static_cast<unsigned short>(m > 32767 ? 32767 : m);
    }
    put_frame(i, render.length, w);
    ++render.done;
    if (render.fill >= kChunkFrames * render.out_channels * 2 || render.done == render.length)
        return flush_chunk() && next_segment();
    return true;
}
// Worker: count or write up to kJobFrames frames.
void render_step() {
    if (render.failed) return;
    i32 w[2];
    for (u32 k = 0; k < kJobFrames; ++k) {
        if (render.counting) {
            if (!render.pipe.next(w) || render.length >= frame_limit()) {
                begin_write();
                return;
            }
            ++render.length;
            continue;
        }
        if (render.done >= render.length) return;
        if (render.splitting) {
            // A40: the one pass's frames back from the card, each slice cut from them
            if (!in_ready(render.done)) return;
            const opxy::i16* f = render.in_buf + (render.done - render.in_start) * render.in_channels;
            w[0] = static_cast<i32>(f[0]) * 65536;
            w[1] = render.in_channels > 1 ? static_cast<i32>(f[1]) * 65536 : w[0];
        } else {
            if (render.from_preview && render.pipe.read < render.in_total) {
                // A41: the preview's frame the chain reads next (one a frame: no retime runs), in the buffer
                if (!in_ready(render.pipe.read)) return;
                render.pipe.data = render.in_buf - render.in_start * render.in_channels;
            }
            if (!render.pipe.next(w)) {
                if (render.streaming) {
                    // A18: the chain has ended; this is the real length
                    render.length = render.done;
                    flush_chunk();
                    return;
                }
                w[0] = w[1] = 0;   // a count of one more than the write makes: silence
            }
        }
        const u32 i = render.done;
        if (render.target == kToSlices && (!render.split || render.splitting)) {
            // the stage's waveform of the output (A40: the one pass drew it)
            u32 bin = i * kBins / render.length;
            if (bin >= kBins) bin = kBins - 1;
            for (u32 c = 0; c < render.channels && !render.splitting; ++c) {
                const i32 a = w[c] >> 16;
                const u32 m = static_cast<u32>(a < 0 ? -a : a);
                if (m > render.env[bin]) render.env[bin] = static_cast<unsigned short>(m > 32767 ? 32767 : m);
            }
            const u32 start = render.cuts[render.slice], end = render.cuts[render.slice + 1];
            const u32 len = end - start;
            // per-slice fades: the chop fade at both ends
            const u32 fade = render.rate * render.edit.chop_fade / 1000u;
            render.fi = fade < len ? fade : len;
            render.fo = render.fi;
            render.step_in = render.step_out = render.fi ? kUnity / render.fi : 0;
            put_frame(i - start, len, w);
            ++render.done;
            if (render.fill >= kChunkFrames * render.out_channels * 2 || render.done == end) {
                if (!flush_chunk()) return;
            }
            if (render.done == end) {
                file_close(render.file);
                render.file = nullptr;
                ++render.slice_done;
                if (++render.slice < render.cut_count) {
                    const u32 frames = render.cuts[render.slice + 1] - render.cuts[render.slice];
                    render.slice_frames[render.slice] = frames;
                    if (!open_output(render.slice_paths[render.slice], frames)) return;
                }
            }
            continue;
        }

        if (!write_out(w)) return;
    }
}
void render_job(u32) { render_step(); }
void render_finish(bool keep) {
    if (render.file) file_close(render.file);
    render.file = nullptr;
    // A40, A41: the reader; the one pass's temporary file, whatever came of the slices
    if (render.in_file) file_close(render.in_file);
    render.in_file = nullptr;
    if (render.in_buf) release(render.in_buf);
    render.in_buf = nullptr;
    if (render.split && render.in_path[0]) remove_file(render.in_path);
    if (render.chunk) release(render.chunk);
    render.chunk = nullptr;
    if (render.object) host::release_sample(render.key);
    render.object = nullptr;
    if (!keep) {
        if (render.target == kToSave && render.temporary[0]) remove_file(render.temporary);
        if (render.target == kToPreview && render.final_path[0]) remove_file(render.final_path);
        if (render.segmented) {
            if (render.file) file_close(render.file);
            render.file = nullptr;
            char path[112];
            for (u32 k = 0; k <= render.segments; ++k) {
                segment_path(path, sizeof path, render.final_path, k);
                remove_file(path);
            }
        }
        if (render.target == kToSlices)
            for (u32 k = 0; k < render.slice_done; ++k) remove_file(render.slice_paths[k]);
    }
}
// A40: the one pass is on the card at its real length: the slices are cut from it, read back in order.
void begin_split() {
    if (render.file) file_close(render.file);
    render.file = nullptr;
    if (!render.length) { fail("nothing to save"); return; }
    render.splitting = true;
    render.streaming = false;
    render.in_total = render.length;
    render.in_channels = render.out_channels;   // the chain's own channels
    render.in_segmented = false;
    render.in_start = render.in_frames = 0;
    render.in_buf = static_cast<opxy::i16*>(allocate(kInFrames * render.in_channels * 2));
    if (!render.in_buf) { fail("no memory"); return; }
    render.chans = render.split_chans;
    render.out_channels = render.split_out;
    render.touched = false;   // dithered once, in the one pass
    render.fill = 0;
    render.done = 0;
    log(__LINE__, "slices: %d frames rendered once in %d ms; cutting them", static_cast<int>(render.length),
        static_cast<int>(now_ms() - render.started_ms));
    begin_slices(render.length, render.length);
}
void render_last(u32) {
    if (render.split && !render.splitting) {
        begin_split();
        return;   // after_render carries on: the slices are the rest of the write
    }
    if (render.streaming && render.segmented) {
        if (render.file) file_close(render.file);   // A23: the counts sent carry the real length
        render.file = nullptr;
        if (!render.length) { fail("nothing to save"); return; }
    } else if (render.streaming) {
        // A18: the header with the real length
        if (render.file) file_close(render.file);
        render.file = nullptr;
        if (!render.length) { fail("nothing to save"); return; }
        File* f = file_open(render.target == kToSave ? render.temporary : render.final_path, "r+b");
        bool ok = f != nullptr;
        if (f) {
            wav_header(header, render.length, render.out_channels, render.rate);
            ok = file_write(header, 1, kHeaderBytes, f) == kHeaderBytes;
            file_close(f);
        }
        if (!ok) {
            header_patch_failed = true;
            fail("couldn't rewrite the header");
            return;
        }
    }
    if (render.target == kToSlices && render.new_kit) {
        if (render.file) file_close(render.file);
        render.file = nullptr;
        if (!write_kit_patch()) {
            fail("couldn't write the kit");   // the cleanup removes the slices
            return;
        }
    }
    if (render.target == kToSave) {
        if (render.file) file_close(render.file);
        render.file = nullptr;
        if (!rename_file(render.temporary, render.final_path)) {
            fail("couldn't rename");
            return;
        }
    }
    render_finish(true);
}

// ------------------------------------------------------------ the PCM (A9)
u32 open_key, stale_key, release_key;
const void* open_object;
bool open_ok, open_unwanted;
CacheState open_state;
void open_job(u32) {
    open_ok = false;
    const Borrowed b = borrow(open_key);
    open_object = b.object;
    open_state = b.state;
    open_unwanted = b.unwanted;
    if (!open_object) return;
    const opxy::SampleView view = host::sample_view(open_object);
    if (!view.data || !view.frames) {
        host::release_sample(open_key);
        open_object = nullptr;
        return;
    }
    u32 peak = 0;
    for (u32 b = 0; b < kBins; ++b) {
        const u32 f0 = b * view.frames / kBins;
        u32 f1 = (b + 1) * view.frames / kBins;
        if (f1 <= f0) f1 = f0 + 1;
        if (f1 > view.frames) f1 = view.frames;
        u32 m = 0;
        for (u32 f = f0; f < f1; ++f)
            for (u32 c = 0; c < view.channels; ++c) {
                const i32 v = view.data[f * view.channels + c];
                const u32 a = v < 0 ? static_cast<u32>(-v) : static_cast<u32>(v);
                if (a > m) m = a;
            }
        studio.env[b] = static_cast<unsigned short>(m > 32767 ? 32767 : m);
        if (m > peak) peak = m;
    }
    studio.peak = peak;
    studio.view = view;
    open_ok = true;
}
void release_job(u32) { host::release_sample(release_key); }
void stale_job(u32) { host::release_sample(stale_key); }
void open_done(u32);
void chop_cuts();
void ask_pcm() {
    studio.loading = true;
    studio.pcm_failed = false;
    open_key = studio.key;
    queue(open_job, open_done);
}
Edit edit_from(const unsigned char* entry);
void open_done(u32) {
    studio.loading = false;
    if (studio.mode == kClosed || open_key != studio.key) {
        if (open_object) {
            stale_key = open_key;
            queue(stale_job, 0);
        }
        if (studio.mode != kClosed) ask_pcm();
        return;
    }
    studio.object = open_object;
    studio.ready = open_ok;
    if (open_ok)
        log(__LINE__, "pcm of %s borrowed: %d frames, %d ch, %s by stock", studio.path, static_cast<int>(studio.view.frames),
            static_cast<int>(studio.view.channels), open_unwanted ? "unwanted" : "wanted");
    studio.pcm_failed = !open_ok;
    studio.retry_ms = now_ms() + 300;
    if (open_ok && studio.file_only && !frames_of(studio.origin)) {
        // A28: a file's markers are its whole length
        host::set_entry_whole(studio.origin, studio.view.frames);
        studio.edit = edit_from(studio.origin);
        studio.opened = studio.edit;
    }
    if (studio.ready && studio.page == kChop) chop_cuts();
    ++studio.generation;
}
void drop_pcm() {
    if (studio.object) {
        release_key = studio.key;
        queue(release_job, 0);
    }
    studio.object = nullptr;
    studio.ready = false;
}
void load_pcm() {
    drop_pcm();
    studio.key = host::sample_key(studio.path);
    if (!studio.loading) ask_pcm();
}

// ------------------------------------------------------------ chop's cuts
u32 output_length() {
    if (studio.out_valid && studio.rendered == studio.change) return studio.out_frames;
    const u32 n = studio.edit.end - studio.edit.start;
    if (live_ok(studio.edit)) return tape_frames(n, studio.edit.stretch);   // A24: what the chain makes
    return dsp::time_active(settings_of(studio.edit)) ? n * studio.edit.stretch / 100u : n;
}
u32 total_frames() {
    if (studio.ready) return studio.view.frames;
    const i32 f = frames_of(studio.origin);
    return f > 0 ? static_cast<u32>(f) : 1;
}
u32 env_max(u32 f0, u32 f1) {
    const u32 total = total_frames();
    u32 b0 = f0 * kBins / total, b1 = f1 > f0 ? (f1 - 1) * kBins / total : b0;
    if (b1 >= kBins) b1 = kBins - 1;
    if (b0 > b1) b0 = b1;
    u32 m = 0;
    for (u32 b = b0; b <= b1; ++b)
        if (studio.env[b] > m) m = studio.env[b];
    return m;
}
// a * n / length for frames up to kSaveMaxFrames, in 32 bits: a as a Q16 fraction of
// length, then n * q16 / 2^16 split so no product passes 2^32 (a, n, length < 2^22).
u32 scale_frames(u32 a, u32 n, u32 length) {
    if (!length || n == length) return a;
    if (a >= length) return n;
    u32 q16;
    if (length < 65536) q16 = (a << 16) / length;
    else if (a < (1u << 20)) q16 = (a << 12) / (length >> 4);
    else q16 = (a << 8) / (length >> 8);
    if (q16 > 65536) q16 = 65536;
    const u32 out = (n >> 16) * q16 + (((n & 0xffffu) * q16) >> 16);
    return out > n ? n : out;
}
// The output's amplitude over output frames [f0, f1): the last render's, or the
// source between the markers (reversed if asked) when nothing is rendered.
u32 out_amp(u32 f0, u32 f1) {
    const u32 n = output_length();
    if (!n) return 0;
    if (studio.out_valid && studio.rendered == studio.change) {
        u32 b0 = f0 * kBins / n, b1 = f1 > f0 ? (f1 - 1) * kBins / n : b0;
        if (b1 >= kBins) b1 = kBins - 1;
        u32 m = 0;
        for (u32 b = b0; b <= b1; ++b)
            if (studio.out_env[b] > m) m = studio.out_env[b];
        return m;
    }
    const Edit& e = studio.edit;
    const u32 src_len = e.end - e.start;
    // output frames onto source frames (f * (src / 64 + 1) would pass 2^32 past ~6 s sources)
    const u32 s0 = scale_frames(f0, src_len, n), s1 = scale_frames(f1, src_len, n);
    const u32 a = e.reverse ? e.end - (s1 < src_len ? s1 : src_len) : e.start + s0;
    const u32 b = e.reverse ? e.end - s0 : e.start + (s1 < src_len ? s1 : src_len);
    return env_max(a, b > a ? b : a + 1);
}
// The slices, as output frames: even, or at the strongest onsets.
void chop_cuts() {
    const Edit& e = studio.edit;
    const u32 n = output_length();
    u32 count = e.slices < 2 ? 2 : e.slices > kMaxSlices ? kMaxSlices : e.slices;
    studio.cuts[0] = 0;
    if (e.chop_mode == 0 || n < 256) {
        for (u32 k = 1; k < count; ++k) studio.cuts[k] = n / count * k + n % count * k / count;
    } else {
        // onset strength over 256 steps: rise above the mean of the four before
        const u32 kSteps = 256;
        static u32 amp[kSteps], strength[kSteps];
        static bool taken[kSteps];
        for (u32 i = 0; i < kSteps; ++i) taken[i] = false;
        for (u32 i = 0; i < kSteps; ++i) amp[i] = out_amp(n / kSteps * i, n / kSteps * (i + 1));
        u32 top = 1;
        for (u32 i = 0; i < kSteps; ++i) {
            u32 mean = 0;
            for (u32 j = 1; j <= 4; ++j) mean += i >= j ? amp[i - j] : 0;
            mean /= 4;
            strength[i] = amp[i] > mean ? amp[i] - mean : 0;
            if (strength[i] > top) top = strength[i];
        }
        const u32 floor_strength = top * (100u - e.sens) / 100u;
        const u32 gap = kSteps / (2 * count) + 1;
        u32 found = 1;
        while (found < count) {
            u32 best = 0, at = 0;
            for (u32 i = 1; i < kSteps; ++i) {
                if (taken[i] || strength[i] < best || strength[i] < floor_strength || !strength[i]) continue;
                bool clear = true;
                for (u32 k = 1; k < found && clear; ++k) {
                    const u32 step = studio.cuts[k] / (n / kSteps ? n / kSteps : 1);
                    clear = (i > step ? i - step : step - i) >= gap;
                }
                if (clear && strength[i] > best) {
                    best = strength[i];
                    at = i;
                }
            }
            if (!at) break;
            taken[at] = true;
            studio.cuts[found++] = n / kSteps * at;
        }
        if (found < 2) {
            // no onsets above the sensitivity: even slices
            for (u32 k = 1; k < count; ++k) studio.cuts[k] = n / count * k + n % count * k / count;
            found = count;
        }
        count = found;
        // in order
        for (u32 a = 1; a < count; ++a)
            for (u32 b = a + 1; b < count; ++b)
                if (studio.cuts[b] < studio.cuts[a]) {
                    const u32 t = studio.cuts[a];
                    studio.cuts[a] = studio.cuts[b];
                    studio.cuts[b] = t;
                }
    }
    studio.cuts[count] = n;
    studio.cut_count = count;
    for (u32 k = 0; k <= count; ++k)
        studio.cut_q16[k] = n >= 256 ? (studio.cuts[k] << 8) / ((n >> 8) + 1) : studio.cuts[k] * 65536u / (n ? n : 1);
    studio.cut_q16[count] = 65536;
}

// -------------------------------------------------------- live preview (A4)
// The key no longer plays a preview file (or never will: the studio moved on).
void forget_preview() {
    studio.showing_preview = false;
    studio.loading_preview = false;
    studio.preview_lost = false;
}
u32 shown_gain() {
    if (studio.edit.normal && studio.peak) return normalise_gain(studio.peak, kNormalTarget[studio.edit.normal - 1]);
    return level_gain(studio.edit.level);
}
i32 live_level() {
    const Edit& e = studio.edit;
    if (!e.normal) return e.level;
    if (!studio.ready || !studio.peak) return 0;
    const u32 want = normalise_gain(studio.peak, kNormalTarget[e.normal - 1]);
    i32 db = kLevelMin;
    for (i32 d = kLevelMin; d <= kLevelMax; ++d)
        if (level_gain(d) <= want) db = d;
    return db;
}
// The auditioned chop slice as output frames, or the whole output.
void audition(u32& a, u32& b) {
    a = 0;
    b = output_length();
    if (studio.page == kChop && studio.edit.slice && studio.edit.slice <= studio.cut_count) {
        a = studio.cuts[studio.edit.slice - 1];
        b = studio.cuts[studio.edit.slice];
    }
}
// A17: the key holds the first part of the current edit while the rest renders.
bool quick_shown() { return studio.quick_change == studio.change && studio.rendered != studio.change; }
void flush_live(bool) { studio.live_dirty = false; }
void touch_live() { studio.live_dirty = true; }
u32 live_rev;   // A32: bumped by every touch; a playing live chain follows it (follow_live)
void touched() {
    ++live_rev;
    // A32: with beat retime, a change the retime doesn't hear keeps the preview: it plays live over it
    const bool keep = preview_render(studio.edit) && studio.time_valid && same_time(studio.edit, studio.time_last) &&
                      preview_render(studio.time_last);
    studio.time_last = studio.edit;
    studio.time_valid = true;
    if (!keep) {
        ++studio.change;
        studio.changed_ms = now_ms();
    }
    if (studio.page == kChop || studio.edit.slice) chop_cuts();
    touch_live();
    ++studio.generation;
}

// -------------------------------------------------------------- open, close
Edit edit_from(const unsigned char* entry) {
    const opxy::SampleRegion r = host::entry_region(entry);
    Edit e = {};
    neutral(e);
    e.start = r.begin;
    e.end = r.end > r.begin ? r.end : static_cast<u32>(frames_of(entry));
    e.level = host::entry_level(entry);
    if (e.level < kLevelMin) e.level = kLevelMin;
    if (e.level > kLevelMax) e.level = kLevelMax;
    e.reverse = r.reverse ? 1 : 0;
    return e;
}
void set_card(const char* a, const char* b, u32 ms) {
    copy_text(studio.card1, a, sizeof studio.card1);
    copy_text(studio.card2, b, sizeof studio.card2);
    studio.card_until = ms ? now_ms() + ms : 0;
}
void clear_card() {
    studio.card1[0] = studio.card2[0] = 0;
    studio.card_until = 0;
}
// Worker: remove preview files [from, to) of a track. Two sets of globals: the
// files the key has moved past (A13) and the sweep when the studio closes, so a
// close never overwrites a removal still queued.
u32 preview_serial;                    // the last preview serial since boot (A11)
struct Removal { u32 track, from, to; } passed, sweep;
void remove_range(const Removal& r) {
    char path[64];
    const u32 from = r.to - r.from > kSweepMax ? r.to - kSweepMax : r.from;
    char part[64];
    for (u32 n = from; n < r.to; ++n) {
        scratch_path(path, sizeof path, r.track, n);
        remove_file(path);
        // A23: its segments, while there are any
        for (u32 k = 0; k < 16; ++k) {
            segment_path(part, sizeof part, path, k);
            if (!file_exists(part)) break;
            remove_file(part);
        }
    }
}
void passed_job(u32) { remove_range(passed); }
// Previews an earlier boot left (a power-off with the studio open): the first time a track's
// studio opens each boot, its names t<track>-1..64 (and their segments) go, on the worker, before
// any render of this opening (the queue runs in order). Serials restart at 1 every boot.
Removal leftovers;
bool leftovers_swept[8];
bool bounces_swept;
void bounce_name(char* out, u32 size, u32 serial, const char* tail);
void leftovers_job(u32) {
    remove_range(leftovers);
    if (!bounces_swept) {
        // bounces an earlier boot left: b1..b16 and their .tmp and .fail (serials restart at 1)
        bounces_swept = true;
        char path[64];
        for (u32 n = 1; n <= 16; ++n) {
            bounce_name(path, sizeof path, n, ".wav");
            remove_file(path);
            bounce_name(path, sizeof path, n, ".tmp");
            remove_file(path);
            bounce_name(path, sizeof path, n, ".fail");
            remove_file(path);
        }
    }
}
void sweep_job(u32) { remove_range(sweep); }
void queue_sweep(u32 track) {
    sweep.track = track;
    sweep.from = studio.serial_first;
    sweep.to = preview_serial + 1;
    studio.serial_first = sweep.to;
    if (sweep.from < sweep.to) queue(sweep_job, 0);
}
void stop_play();
void begin_studio(u32 track, bool file);
void put_back_original() {
    // the player stopped (the track was never changed, A16)
    stop_play();
    forget_preview();
}
bool open_studio(u32 track, u32 engine, u32 slot) {
    unsigned char* entry = region_entry(track, slot);
    if (!entry || frames_of(entry) <= 0) { popup("no sample on this key"); return false; }
    const char* path = host::entry_path(entry);
    u32 length = 0;
    while (length < kPathBytes && path[length]) ++length;
    if (!length || length >= kPathBytes) { popup("no sample on this key"); return false; }
    if (starts_with(path, kScratchFolder)) { popup("a preview file: open the original"); return false; }
    studio.track = track;
    studio.engine = engine;
    studio.slot = slot;
    studio.on_key = false;
    copy_text(studio.path, path, sizeof studio.path);
    if (pending.valid && pending.track == track && pending.slot == slot && same_text(pending.path, path, kPathBytes)) {
        studio.edit = pending.edit;
        studio.opened = pending.opened;
        copy_bytes(studio.origin, pending.origin, kEntryBytes);
    } else {
        copy_bytes(studio.origin, entry, kEntryBytes);
        if (snapshot.valid && snapshot.track == track && snapshot.slot == slot &&
            same_text(snapshot.path, path, kPathBytes))
            copy_bytes(studio.origin, snapshot.entry, kPadBytes);
        studio.edit = edit_from(studio.origin);
        studio.opened = studio.edit;
    }
    const bool kept = pending.valid && same_edit(studio.edit, pending.edit);
    pending.valid = false;
    log(__LINE__, "open %s: track %d key %d, %d frames, markers %d..%d", studio.path, static_cast<int>(track),
        static_cast<int>(slot), frames_of(entry), static_cast<int>(studio.edit.start), static_cast<int>(studio.edit.end));
    // what decides pitch: the key's tuning (stock plays the key by it; the studio plays at the file's own
    // pitch) and the edit's speed (tape: pitch with it), and whether a kept edit came back
    log(__LINE__, "open: root %d fine %d, speed %d %s%s", static_cast<int>(host::entry_root(entry)),
        static_cast<int>(host::entry_fine(entry)), static_cast<int>(studio.edit.stretch), studio.edit.beat ? "beat" : "tape",
        kept ? ", the edit kept from before" : "");
    begin_studio(track, false);
    return true;
}
// The rest of an opening, a key's or a file's (A28: a file holds no track).
void begin_studio(u32 track, bool file) {
    studio.file_only = file;
    studio.mode = kOpen;
    waveform_said = false;
    axis_frames = 0;
    axis_moving = flashing = false;
    header_state.reset();
    header_ms = 0;
    studio.page = kTrim;
    studio.row = 0;
    studio.compare = studio.leave_after_save = false;
    studio.save_pending = false;
    forget_preview();
    studio.serial_first = preview_serial + 1;
    studio.shown_serial = 0;
    if (track < 8 && !leftovers_swept[track] && !busy) {
        leftovers_swept[track] = true;
        leftovers.track = track;
        leftovers.from = 1;
        leftovers.to = 65;
        queue(leftovers_job, 0);
    }
    studio.failed_change = studio.failures = 0;
    studio.quick_change = 0;
    studio.time_valid = false;   // A32
    studio.rendered = 0;
    studio.out_valid = false;
    studio.zoom_marker = 0;
    studio.gesture = -1;
    studio.undo_count = 0;
    clear_card();
    // A28: the keys play the edit on any track, a file's too, and the track's own engine is held
    // silent while the studio is open (A31: a file for a key plays on the key, as a key's sample)
    note_seen = track >= 8 ? 0 : track_notes();
    live_sent_valid = false;   // A24: the first play sends the whole set
    sends_valid = false;
    if (track < 8) send_hold(track + 1);
    load_pcm();
    touched();
}
// A28: open the browser's highlighted file on a track without a sample key. A31: or for the
// selected key of a drum sampler or multisampler (engine, slot), where the library highlights a file
// without putting it on the key: the keys play it, save puts it on the key, slices can go on the kit.
// A49 A file on no key plays from its own root: the 'smpl' chunk's unity note and pitch fraction
//    (stock's instrument samples carry one: ensemble/c2.wav 36, harmonium/a2.wav 45), else middle C.
//    Read on the worker when the file opens, through its chunks (no seek), at most kRootScanBytes;
//    notes struck before it lands play from middle C.
const u32 kRootScanBytes = 16u << 20;
char root_path[kPathBytes];
u32 root_serial, root_job_serial;
bool root_found;
i32 root_cents;
unsigned char root_scratch[4096];
struct FileReader {
    File* file;
    u32 operator()(void* to, u32 n) { return file_read(to, 1, n, file); }
};
void root_job(u32 serial) {
    root_found = false;
    root_cents = 6000;
    if (File* f = file_open(root_path, "rb")) {
        FileReader read = {f};
        root_found = wav_root_cents(read, root_cents, kRootScanBytes, root_scratch, sizeof root_scratch);
        file_close(f);
    }
    root_job_serial = serial;
}
void root_done(u32 serial) {
    if (serial != root_serial || root_job_serial != serial || !studio.file_only || studio.on_key) return;
    studio.file_root = root_found ? root_cents : 6000;
    if (root_found)
        log(__LINE__, "open: the file's root, smpl unity %d (+%d cents): notes play from it", root_cents / 100,
            root_cents % 100);
    else
        log(__LINE__, "open: no root in the file: notes play from middle C (60)");
}
bool open_file(u32 track, const char* path, u32 engine = 0, u32 slot = 0) {
    if (starts_with(path, kScratchFolder)) { popup("a preview file: open the original"); return false; }
    host::stop_audition();   // stop stock's audition of it (as stock does on leaving the list)
    unsigned char blank[kEntryBytes];
    for (u32 i = 0; i < kEntryBytes; ++i) blank[i] = 0;
    const unsigned char* key = engine ? region_entry(track, slot) : nullptr;
    studio.on_key = key != nullptr;
    if (key) copy_bytes(studio.key_entry, key, kEntryBytes);
    studio.track = track;
    studio.engine = key ? engine : 0;
    studio.slot = key ? slot : 0;
    copy_text(studio.path, path, sizeof studio.path);
    copy_bytes(studio.origin, blank, kEntryBytes);
    studio.edit = edit_from(studio.origin);   // markers set once the PCM is in (open_done)
    studio.opened = studio.edit;
    pending.valid = false;
    studio.file_root = 6000;
    if (studio.on_key)
        log(__LINE__, "open %s: the highlighted file for track %d key %d", studio.path, static_cast<int>(track),
            static_cast<int>(slot));
    else
        log(__LINE__, "open %s: file mode on track %d (no key)", studio.path, static_cast<int>(track));
    begin_studio(track, true);
    if (!studio.on_key) {
        copy_text(root_path, path, sizeof root_path);
        host::post(root_job, root_done, ++root_serial);   // A49: once the studio is open on it
    }
    return true;
}
bool dirty() { return !same_edit(studio.edit, studio.opened); }
bool bounce_running();
void end_bounce(const char* why, const char* detail);
void close_studio(bool keep) {
    if (studio.mode == kClosed) return;
    if (bounce_running()) end_bounce("print stopped", "the studio closed");
    flush_live(true);
    put_back_original();
    release_player();
    send_hold(0);   // A22: the track sounds again
    pending.valid = keep && dirty();
    if (pending.valid) {
        pending.track = studio.track;
        pending.slot = studio.slot;
        copy_text(pending.path, studio.path, sizeof pending.path);
        pending.edit = studio.edit;
        pending.opened = studio.opened;
        copy_bytes(pending.origin, studio.origin, kEntryBytes);
    }
    drop_pcm();
    if (!busy) queue_sweep(studio.track);   // else after_render sweeps once the render ends
    if (library_stale) relist_library();   // A35: the saves are on the card; a preview render doesn't matter
    log(__LINE__, "closed%s", pending.valid ? ", edit kept for later" : "");
    studio.mode = kClosed;
    studio.compare = false;
    clear_card();
}
void discard_and_close() {
    put_back_original();
    studio.live_dirty = false;
    studio.edit = studio.opened;
    close_studio(false);
}

// ----------------------------------------------------------------- undo
UndoRecord& push_undo(UndoKind kind) {
    if (studio.undo_count == kUndoDepth) {
        for (u32 i = 1; i < kUndoDepth; ++i) studio.undo[i - 1] = studio.undo[i];
        --studio.undo_count;
    }
    UndoRecord& r = studio.undo[studio.undo_count++];
    r.kind = kind;
    r.edit = studio.edit;
    r.opened = studio.opened;
    copy_text(r.path, studio.path, sizeof r.path);
    copy_bytes(r.origin, studio.origin, kEntryBytes);
    return r;
}
void undo_step() {
    if (!studio.undo_count) return;
    const UndoRecord& r = studio.undo[--studio.undo_count];
    if (r.kind == kFileStep) {
        unsigned char* entry = region_entry(studio.track, studio.slot);
        if (entry) {
            copy_bytes(entry, r.entry, kEntryBytes);
            send_regions(studio.track, true);
        }
        copy_text(studio.path, r.path, sizeof studio.path);
        copy_bytes(studio.origin, r.origin, kEntryBytes);
        studio.opened = r.opened;
        studio.edit = r.edit;
        forget_preview();
        load_pcm();
        set_card("back to", base_name(studio.path), 1500);
    } else if (r.kind == kKitStep) {
        if (kit_undo.valid && kit_undo.track == studio.track) {
            for (u32 k = 0; k < kit_undo.count; ++k) {
                unsigned char* entry = region_entry(kit_undo.track, kit_undo.notes[k]);   // A28: by note
                if (entry) copy_bytes(entry, kit_undo.entries[k], kEntryBytes);
            }
            send_regions(kit_undo.track, true);
            log(__LINE__, "undo: this kit, %d keys as before from key %d", static_cast<int>(kit_undo.count),
                static_cast<int>(kit_undo.count ? kit_undo.notes[0] : 0));
            kit_undo.valid = false;
            set_card("keys as before", "", 1500);
        }
        studio.edit = r.edit;
    } else {
        studio.edit = r.edit;
    }
    studio.gesture = -1;
    touched();
}

// ----------------------------------------------------------------- saving
void after_render(u32);
void failed_done(u32);
void cleanup_job(u32) { render_finish(false); }
void continue_render() {
    if (render.failed) {
        queue(cleanup_job, failed_done);
        return;
    }
    if (render.counting || render.done < render.length) {
        queue(render_job, after_render);
        return;
    }
    queue(render_last, after_render);
}
bool render_finished() { return !render.counting && render.done >= render.length && !render.file && !render.chunk; }
// "saved in samples/user": the folder a save went to, as the card shows it.
void saved_where(const char* path, char* out, u32 size) {
    char folder[kPathBytes];
    path_folder(path, folder, sizeof folder);
    const char* shown = starts_with(folder, "/fat32/") ? folder + 7 : folder;
    u32 len = 0;
    while (shown[len]) ++len;
    if (len > 14) shown = base_name(folder);
    char cut[15];
    copy_text(cut, shown, sizeof cut);   // 14 characters at most: the card's width at 20 px
    u32 n = 0;
    out[0] = 0;
    append(out, size, n, "saved in ");
    append(out, size, n, cut);
}
void load_saved() {
    if (studio.file_only && !studio.on_key) {
        // A28: no key to put it on: the file, and where it went
        char where[24];
        saved_where(render.final_path, where, sizeof where);
        set_card(where, base_name(render.final_path), 4000);
        log(__LINE__, "saved %s (file mode: on no key)", render.final_path);
        return;
    }
    unsigned char* entry = region_entry(render.track, render.slot);
    if (!entry) return;
    UndoRecord& r = push_undo(kFileStep);
    // the original, even if a preview was playing (A31: the key's own, for a highlighted file)
    const unsigned char* before = studio.on_key ? studio.key_entry : studio.origin;
    copy_bytes(r.entry, before, kEntryBytes);
    if (!assign_file(render.track, render.engine, render.slot, render.final_path, key_frames(render.length),
                     host::entry_root(before))) {
        log(__LINE__, "stock refused the path %s", render.final_path);
        --studio.undo_count;
        set_card("saved, not loaded", base_name(render.final_path), 2500);
        return;
    }
    send_regions(render.track, true);
    studio.file_only = studio.on_key = false;   // A31: the key's sample now
    copy_text(studio.path, render.final_path, sizeof studio.path);
    unsigned char* now = region_entry(render.track, render.slot);
    if (now) copy_bytes(studio.origin, now, kEntryBytes);
    Edit plain = {};
    neutral(plain);
    plain.end = key_frames(render.length);
    studio.edit = studio.opened = plain;
    forget_preview();
    studio.live_dirty = false;
    studio.rendered = 0;
    studio.out_valid = false;
    ++studio.change;
    load_pcm();
    axis_frames = 0;
    landed(key_frames(render.length));
    char where[24];
    saved_where(render.final_path, where, sizeof where);
    if (render.length > kMaxFrames) {
        // the file is whole; stock loads 20 s of it on a key (A12)
        char second[24];
        u32 n = 0;
        second[0] = 0;
        append(second, sizeof second, n, "whole; key plays 20 s");
        set_card(where, second, 5000);
        log(__LINE__, "  %s is %d frames; its key holds the first %d", render.final_path,
            static_cast<int>(render.length), static_cast<int>(kMaxFrames));
    } else if (print_dry) {
        char second[24];
        u32 m = 0;
        second[0] = 0;
        append(second, sizeof second, m, "dry: ");
        append(second, sizeof second, m, effect_name(dry_engine));
        append(second, sizeof second, m, " not printed");
        set_card(where, second, 5000);   // A29: only a live edit bounces
    } else {
        set_card(where, base_name(render.final_path), 4000);
    }
}
void load_kit() {
    if (render.new_kit) {
        // A30: the kit is a preset now; the studio stays on the original
        log(__LINE__, "kit: %d slices as a new kit in %s (keys %d up)", static_cast<int>(render.cut_count),
            render.kit_folder, static_cast<int>(kKitFirstKey));
        set_card("new kit saved", base_name(render.kit_folder), 4000);
        // A37: stock's preset index learns of it now, not when the studio closes: the studio no longer
        // sees the browser screen once the library leaves it (the SDK hands it the library's frames
        // only), so a close can come after the preset browser has already opened without the kit.
        rescan_presets();
        return;
    }
    if (render.engine != kDrumSampler || !slices_to_kit) {
        char line[24];
        u32 n = 0;
        line[0] = 0;
        append_number(line, sizeof line, n, render.cut_count);
        append(line, sizeof line, n, " slices saved");
        set_card(line, base_name(render.slice_paths[0]), 2500);
        return;
    }
    // a drum sampler: the slices on the kit's pads from the selected one up, in key order (keys 53 up).
    // A pad is a key with its own region (the first key on that region; A42: a kit with fewer pads gives
    // some keys another key's region, and those aren't pads), or a key with no region at all: a fresh
    // kit's empty pad (stock's new kit sets up only the pads it has loaded), given its slice as stock's
    // library gives a sample to an empty pad. render.slot is the selected key's note (A28).
    unsigned char* found[24];
    for (u32 k = 0; k < 24; ++k) found[k] = region_entry(render.track, 53 + k);
    u32 pad_keys[24];
    bool fresh_pad[24];
    u32 pads = 0, lacking = 0, sharing = 0;
    for (u32 k = 0; k < 24; ++k) {
        bool shared = false;
        for (u32 j = 0; found[k] && j < k && !shared; ++j) shared = found[j] == found[k];
        if (shared) {
            ++sharing;
            continue;
        }
        if (!found[k]) ++lacking;
        fresh_pad[pads] = !found[k];
        pad_keys[pads++] = 53 + k;
    }
    u32 first = pads;
    for (u32 q = 0; q < pads; ++q)
        if (pad_keys[q] == render.slot) first = q;
    if (first >= pads || fresh_pad[first]) {
        log(__LINE__, "kit: the selected key %d isn't one of its pads: the slices stay files", static_cast<int>(render.slot));
        set_card("slices saved as files", "kit keys not found", 3000);
        return;
    }
    u32 count = pads - first < render.cut_count ? pads - first : render.cut_count;   // the pads from the selected one up
    log(__LINE__, "kit: %d pads (%d keys without a region, %d sharing another's): %d slices from key %d",
        static_cast<int>(pads), static_cast<int>(lacking), static_cast<int>(sharing), static_cast<int>(count),
        static_cast<int>(render.slot));
    kit_undo.valid = true;
    kit_undo.track = render.track;
    kit_undo.first = first;
    kit_undo.count = count;
    for (u32 k = 0; k < count; ++k) {
        const u32 note = pad_keys[first + k];
        kit_undo.notes[k] = note;
        unsigned char* entry = region_entry(render.track, note);
        if (entry) copy_bytes(kit_undo.entries[k], entry, kEntryBytes);
        else for (u32 i = 0; i < kEntryBytes; ++i) kit_undo.entries[k][i] = 0;   // undo leaves the pad empty
        if (k == 0) copy_bytes(kit_undo.entries[0], studio.on_key ? studio.key_entry : studio.origin, kEntryBytes);
        const bool placed_ok = assign_file(render.track, kDrumSampler, note, render.slice_paths[k],
                                           key_frames(render.slice_frames[k]), 60);
        if (fresh_pad[first + k]) {
            // a pad of its own now; if the file went into an earlier pad's region instead, that pad
            // gets its own slice back and the rest stay files
            unsigned char* made = region_entry(render.track, note);
            u32 clash = k;
            for (u32 j = 0; made && j < k && clash == k; ++j)
                if (made == region_entry(render.track, pad_keys[first + j])) clash = j;
            if (!placed_ok || !made || clash < k) {
                log(__LINE__, "kit: key %d took no region of its own: slices %d up stay files", static_cast<int>(note),
                    static_cast<int>(k + 1));
                if (clash < k) {
                    assign_file(render.track, kDrumSampler, pad_keys[first + clash], render.slice_paths[clash],
                                key_frames(render.slice_frames[clash]), 60);
                    if (unsigned char* back = region_entry(render.track, pad_keys[first + clash])) host::set_entry_tuning(back, 60, 0);
                }
                count = k;
                kit_undo.count = placed_ok && made && clash == k ? k + 1 : k;
                break;
            }
        }
        // A34 (else slices on the kit play two octaves up): the drum
        // sampler's assignment leaves the key's root and fine as they were; a slice is at its own pitch
        // on root 60, fine 0, as stock's kits (zebra.json: pitch.keycenter 60, tune 0)
        unsigned char* placed = region_entry(render.track, note);
        if (placed) {
            if (k < 3)
                log(__LINE__, "kit: key %d was root %d fine %d; now 60, 0", static_cast<int>(note),
                    static_cast<int>(host::entry_root(placed)), static_cast<int>(host::entry_fine(placed)));
            host::set_entry_tuning(placed, 60, 0);
        }
    }
    send_regions(render.track, true);
    push_undo(kKitStep);
    char line[24];
    u32 n = 0;
    line[0] = 0;
    append_number(line, sizeof line, n, count);
    append(line, sizeof line, n, " slices on keys");
    if (count < render.cut_count) {
        // A27: the kit holds 24 keys; the rest are files
        char second[24];
        u32 m = 0;
        second[0] = 0;
        append_number(second, sizeof second, m, render.cut_count - count);
        append(second, sizeof second, m, " more as files only");
        set_card(line, second, 4000);
    } else {
        set_card(line, base_name(render.slice_paths[0]), 2500);
    }
    // A43: on the first slice (here 0.15 s) the studio would show a single, very compressed
    // waveform at the left; it stays on the source and its edit, as New kit does (A30). The keys
    // hold the slices; the studio plays the source by its hash, never through the key (flush_live writes nothing)
    log(__LINE__, "kit: the studio stays on %s", studio.path);
}
void failed_done(u32) {
    if (render.superseded) {
        // a newer edit (A17): no retry, no failure counted
        log(__LINE__, "preview %s%s: cancelled after %d of %d frames, %d ms: edited meanwhile", render.final_path,
            render.quick ? " (first part)" : "", static_cast<int>(render.done), static_cast<int>(render.length),
            static_cast<int>(now_ms() - render.started_ms));
        if (studio.mode == kClosed) queue_sweep(render.track);
        barrier();
        busy = 0;
        return;
    }
    log(__LINE__, "render of %s failed: %s, after %d of %d frames", render.source, render.why,
        static_cast<int>(render.done), static_cast<int>(render.length));
    if (render.target != kToPreview) {
        set_card("not saved", render.why, 2500);
        if (studio.mode == kSaving) studio.mode = kOpen;
        studio.leave_after_save = false;
    } else {
        // tried again after kRetryMs, kRetries times for the same edit (A13)
        if (studio.failed_change != render.change) {
            studio.failed_change = render.change;
            studio.failures = 0;
        }
        if (++studio.failures >= kRetries) studio.rendered = render.change;
        studio.retry_at_ms = now_ms() + kRetryMs;
        studio.out_valid = false;
        if (studio.mode == kClosed) queue_sweep(render.track);
    }
    barrier();
    busy = 0;
}
void after_render(u32) {
    if (render.failed) {
        continue_render();
        return;
    }
    if (!render_finished()) {
        if (render.target == kToPreview &&
            (render.change != studio.change || studio.mode == kClosed || studio.save_pending)) {
            render.superseded = true;   // A17: stop at this job, not at the end
            fail("superseded");
        }
        continue_render();
        return;
    }
    barrier();
    busy = 0;
    if (render.target == kToPreview) {
        const u32 took = now_ms() - render.started_ms;
        if (studio.mode == kClosed) {
            log(__LINE__, "preview %s: done after the studio closed, removed", render.final_path);
            queue_sweep(render.track);   // closed meanwhile: no preview file outlives the studio
            return;
        }
        if (render.quick) {
            // the first kQuickMs on the key while the whole edit renders (A17)
            if (render.change == studio.change) {
                copy_text(studio.preview, render.final_path, sizeof studio.preview);
                studio.preview_frames = render.length;
                studio.preview_channels = render.out_channels;
                studio.preview_streamed = false;
                studio.shown_segmented = false;
                studio.showing_preview = true;
                studio.shown_serial = render.serial;
                studio.quick_change = render.change;
                studio.preview_lost = studio.resent = false;
                studio.swapped_ms = now_ms();
                studio.next_check_ms = studio.swapped_ms + kCheckMs;
                remove_passed();
                load_preview();
                log(__LINE__, "preview %s: first %d frames in %d ms, change %d, ready%s", render.final_path,
                    static_cast<int>(render.length), static_cast<int>(took), static_cast<int>(render.change),
                    studio.compare ? " after compare" : "");
                touch_live();
                if (!studio.compare) {
                    flush_live(true);
                    }
            } else {
                log(__LINE__, "preview %s: first part in %d ms, not used: edited meanwhile", render.final_path,
                    static_cast<int>(took));
            }
            ++studio.generation;
            return;
        }
        if (render.streaming) {
            // A18 binned the output by its estimated length: spread what was filled over the real one
            u32 filled = (render.length * kBins + render.estimate - 1) / render.estimate;
            if (filled > kBins) filled = kBins;
            if (!filled) filled = 1;
            for (u32 b = 0; b < kBins; ++b) studio.out_env[b] = render.env[b * filled / kBins];
        } else {
            for (u32 b = 0; b < kBins; ++b) studio.out_env[b] = render.env[b];
        }
        studio.out_frames = render.length;
        studio.out_valid = true;
        studio.cut_at_limit = render.length >= kMaxFrames;
        studio.rendered = render.change;
        studio.failures = 0;
        if (studio.rendered == studio.change) {
            copy_text(studio.preview, render.final_path, sizeof studio.preview);
            studio.preview_frames = render.length;
            studio.preview_channels = render.out_channels;
            studio.preview_streamed = render.streaming;
            studio.shown_segmented = render.segmented;
            studio.showing_preview = true;
            studio.shown_serial = render.serial;
            studio.preview_lost = studio.resent = false;
            studio.swapped_ms = now_ms();
            studio.next_check_ms = studio.swapped_ms + kCheckMs;
            log(__LINE__, "preview %s: %d frames, %d ch, %d Hz, rendered in %d ms (%d writes, %s), change %d, ready%s",
                render.final_path, static_cast<int>(render.length), static_cast<int>(render.out_channels),
                static_cast<int>(render.rate), static_cast<int>(took), static_cast<int>(render.writes),
                render.streaming ? "one pass" : dsp::time_active(settings_of(render.edit)) ? "counted" : "known length",
                static_cast<int>(render.change), studio.compare ? " after compare" : "");
            if (studio.page == kChop) chop_cuts();
            touch_live();
            if (!studio.compare) {
                // the preview branch of flush_live puts the file on the key (A6) and sends the table
                flush_live(true);
            }
            landed(render.length);
            remove_passed();
            growing_landed(render.serial, render.length);
            load_preview();
        } else {
            log(__LINE__, "preview %s: %d frames in %d ms, not used: edited meanwhile (change %d, now %d)",
                render.final_path, static_cast<int>(render.length), static_cast<int>(took),
                static_cast<int>(render.change), static_cast<int>(studio.change));
        }
        ++studio.generation;
        return;
    }
    {
        // Everything tests/studio_chain.py needs to remake the file.
        const Edit& e = render.edit;
        files_written();
        log(__LINE__, "saved %s -> %s: %d frames, %d ch -> %d, %d Hz (full render, %s, %d ms)", render.source,
            render.target == kToSlices ? render.slice_paths[0] : render.final_path, static_cast<int>(render.length),
            static_cast<int>(render.channels), static_cast<int>(render.out_channels), static_cast<int>(render.rate),
            render.streaming ? "one pass" : "counted or known length", static_cast<int>(now_ms() - render.started_ms));
        log(__LINE__, "  --start %d --end %d%s --gain %d --normalise %d%s --fade-in %d --fade-out %d --chans %d",
            static_cast<int>(e.start), static_cast<int>(e.end), e.reverse ? " --reverse" : "", static_cast<int>(e.level),
            static_cast<int>(e.normal), e.dc ? " --dc" : "", static_cast<int>(e.fade_in), static_cast<int>(e.fade_out),
            static_cast<int>(e.chans));
        log(__LINE__, "  --stretch %d --pitch %d --mode %s --grain %d --rate-index %d --bits-index %d --drive %d "
            "--sweep %d --comp %d", static_cast<int>(e.stretch), static_cast<int>(e.pitch),
            e.beat ? "beat" : "tape", static_cast<int>(e.grain), static_cast<int>(e.rate_index),
            static_cast<int>(e.bits_index), static_cast<int>(e.drive), static_cast<int>(e.sweep), static_cast<int>(e.comp));
        if (e.send1 || e.send2)
            log(__LINE__, "  sends FX I %d, FX II %d, tail %d s: %s", static_cast<int>(e.send1), static_cast<int>(e.send2),
                static_cast<int>(e.tail), render.printed ? "printed live (A45)" : "saved dry (A29, A44)");
        if (render.target == kToSlices)
            for (u32 k = 0; k < render.cut_count; ++k)
                log(__LINE__, "  slice %d: %s from %d, %d frames", static_cast<int>(k + 1), render.slice_paths[k],
                    static_cast<int>(render.cuts[k]), static_cast<int>(render.slice_frames[k]));
    }
    if (studio.mode == kSaving) studio.mode = kOpen;
    if (render.target == kToSlices) load_kit();
    else load_saved();
    if (render.printed && print_capped) set_card("tail cut at 30 s", "still ringing", 5000);   // A47
    if (studio.leave_after_save) close_studio(false);
}
void start_render(Target target, bool quick = false) {
    busy = 1;
    render.quick = quick && target == kToPreview;
    render.superseded = false;
    const Edit& e = studio.edit;
    render.target = target;
    render.change = studio.change;
    render.edit = target == kToPreview && !live_ok(e) ? time_only(e) : e;   // A32: the rest plays live over it
    copy_text(render.source, studio.path, sizeof render.source);
    render.track = studio.track;
    render.engine = studio.engine;
    render.slot = studio.slot;
    render.new_kit = target == kToSlices && slices_new_kit;   // A30
    render.kit_folder[0] = 0;
    render.key = host::sample_key(render.source);
    render.level = render.edit.level;
    render.normal = render.edit.normal;
    render.chans = render.edit.chans;
    render.done = render.written = render.writes = render.length = 0;
    render.synced = render.segments = 0;
    render.segmented = false;
    render.object = nullptr;
    render.file = nullptr;
    render.chunk = nullptr;
    render.failed = false;
    render.why = "";
    render.dither.state = kSeed;
    render.final_path[0] = render.temporary[0] = 0;
    render.started_ms = now_ms();
    // A29: a save renders its sends dry (a live edit with a send above 0 bounces instead)
    if (target != kToPreview) print_dry = false;
    for (u32 k = 0; k < 2 && target == kToSave; ++k) {
        if (!(k ? e.send2 : e.send1)) continue;
        const u32 engine = host::track_engine(14 + k);   // FX I is track 14, FX II track 15
        log(__LINE__, "print: FX %s (engine %d) saved dry", k ? "II" : "I", static_cast<int>(engine));
        print_dry = true;
        dry_engine = engine;
    }
    // A40, A41: the reader's state; a save of a beat retime whose whole preview of this edit is on the card
    // reads that preview instead of retiming again (normalise needs the source's peak: it retimes)
    render.split = render.splitting = false;
    render.printed = false;
    render.in_file = nullptr;
    render.in_buf = nullptr;
    render.in_start = render.in_frames = render.in_file_end = 0;
    render.in_path[0] = 0;
    render.from_preview = target != kToPreview && !live_ok(e) && !e.normal && studio.out_valid &&
                          studio.rendered == studio.change && studio.showing_preview && studio.shown_segmented &&
                          !studio.cut_at_limit && studio.preview_frames && studio.preview_channels;
    if (render.from_preview) {
        copy_text(render.in_path, studio.preview, sizeof render.in_path);
        render.in_total = studio.preview_frames;
        render.in_channels = studio.preview_channels;
        render.in_segmented = true;
        render.rate = studio.view.rate ? studio.view.rate : 44100u;
        log(__LINE__, "save from the preview %s: %d frames, %d ch, the retime done", render.in_path,
            static_cast<int>(render.in_total), static_cast<int>(render.in_channels));
    }
    render.serial = 0;
    if (target == kToPreview) {
        render.serial = ++preview_serial;
        scratch_path(render.final_path, sizeof render.final_path, studio.track, render.serial);
    }
    if (dsp::time_active(settings_of(e)))
        log(__LINE__, "render %s%s: source %d frames, %d ch, %d Hz, edit %d..%d%s, stretch %d, pitch %d, %s, grain %d ms",
            target == kToPreview ? render.final_path : "(save)", render.quick ? " (first part)" : "",
            static_cast<int>(studio.view.frames), static_cast<int>(studio.view.channels), static_cast<int>(studio.view.rate),
            static_cast<int>(e.start), static_cast<int>(e.end), e.reverse ? " reversed" : "", static_cast<int>(e.stretch),
            static_cast<int>(e.pitch), e.beat ? "beat" : "tape", static_cast<int>(e.grain));
    barrier();
    queue(render_first, after_render);
}
void b3(u32 id, i32 value);
void play_live(u32 a, u32 b, bool slice, u32 offset);
void play_live_preview(u32 a, u32 b, bool slice);
// A29: the real-time bounce. File work runs on the worker (choose_paths makes many bridge calls);
// the GUI only sends B3s and watches.
struct Bounce {
    bool on, preparing, polling, found, failed, renamed, paths_ok;
    bool from_preview;                 // A44: a beat retime, printed by playing its preview live
    bool slices;                       // A45: the slice page's save: the print is cut into slices
    u32 out_frames;                    // A45: the edit's 44.1 kHz frames, without the tail
    bool until_quiet, capped;          // A47: a "full" tail; it ran to the cap, still ringing
    u32 got;                           // A47: the frames the audio board wrote (its header)
    u32 serial, total, t0, deadline, poked;
    char result[64], failure[64];
} bounce;
u32 bounce_serials;
bool wants_bounce(const Edit& e) { return e.send1 || e.send2; }   // A29: every effect, Lint included
void bounce_name(char* out, u32 size, u32 serial, const char* tail) {
    u32 k = 0;
    out[0] = 0;
    append(out, size, k, kScratchFolder);
    append(out, size, k, "/b");
    append_number(out, size, k, serial);
    append(out, size, k, tail);
}
// A44: the whole preview of this edit is on the card and loaded on the audio board when it lands (P13).
bool full_preview_ready() {
    return studio.out_valid && studio.rendered == studio.change && studio.showing_preview && studio.shown_segmented &&
           !studio.cut_at_limit && studio.preview_frames && studio.shown_serial;
}
// A44: an edit the print can play live: everything but a beat retime, which needs its whole preview.
bool can_print(const Edit& e) {
    return (live_ok(e) || full_preview_ready()) && (!studio.file_only || studio.on_key);
}
// Worker: the save's name, the scratch folder, no stale result.
void bounce_prepare_job(u32) {
    bounce.paths_ok = bounce.slices || choose_paths();   // A45: the slices take their names when cut
    make_folder(kScratchFolder);
    remove_file(bounce.result);
    remove_file(bounce.failure);
}
void bounce_prepare_done(u32) {
    bounce.preparing = false;
    if (!bounce.on) return;   // cancelled meanwhile
    if (!bounce.paths_ok) {
        bounce.on = false;
        set_card("print failed", "no folder to save in", 3000);
        studio.mode = kOpen;
        return;
    }
    b3(151, static_cast<i32>(bounce.total | (bounce.until_quiet ? 0x80000000u : 0u)));   // the size first (review); A47's flag
    b3(150, static_cast<i32>(bounce.serial));
    bounce.t0 = bounce.poked = now_ms();
    bounce.deadline = bounce.t0 + bounce.total / 44u + 20000u;   // its length in ms, plus 20 s to write it
    const Edit& e = studio.edit;
    if (bounce.from_preview) play_live_preview(0, studio.preview_frames, false);   // A44
    else play_live(e.start, e.end, false, 0);
}
// Worker: has the audio board written the file (or given up)? If written, name it as the save.
void bounce_check_job(u32) {
    bounce.failed = file_exists(bounce.failure);
    bounce.found = !bounce.failed && file_exists(bounce.result);
    if (bounce.found) {
        // A47: the frames written, from its header (a "full" tail ends where the print went quiet)
        File* f = file_open(bounce.result, "rb");
        unsigned char h[kHeaderBytes];
        if (f && file_read(h, 1, kHeaderBytes, f) == kHeaderBytes)
            bounce.got = (static_cast<u32>(h[40]) | static_cast<u32>(h[41]) << 8 | static_cast<u32>(h[42]) << 16 |
                          static_cast<u32>(h[43]) << 24) / 4u;
        if (f) file_close(f);
    }
    if (bounce.found && bounce.slices) {
        bounce.renamed = true;   // A45: cut from where it is (the split removes it)
    } else if (bounce.found) {
        remove_file(render.temporary);
        bounce.renamed = rename_file(bounce.result, render.final_path);
        if (!bounce.renamed) remove_file(bounce.result);
    }
    if (bounce.failed) remove_file(bounce.failure);
}
void bounce_check_done(u32) { bounce.polling = false; }
// Worker: what a stopped or failed bounce leaves.
void bounce_clean_job(u32) {
    if (render.temporary[0]) remove_file(render.temporary);
    remove_file(bounce.result);
    remove_file(bounce.failure);
}
bool bounce_running() { return bounce.on; }
void end_bounce(const char* why, const char* detail) {
    bounce.on = false;
    b3(150, 0);   // the audio board lets its capture go
    stop_play();
    if (!busy) queue(bounce_clean_job, 0);
    log(__LINE__, "bounce %d: %s %s", static_cast<int>(bounce.serial), why, detail);
    set_card(why, detail, 3000);
    if (studio.mode == kSaving) studio.mode = kOpen;
}
bool start_bounce(bool slices = false) {
    const Edit& e = studio.edit;
    if (busy || bounce.preparing) return false;
    render.target = slices ? kToSlices : kToSave;
    copy_text(render.source, studio.path, sizeof render.source);
    const u32 rate = studio.view.rate ? studio.view.rate : 44100u;
    // A44: a beat retime plays its preview, already at its output length
    bounce.from_preview = !live_ok(e);
    bounce.slices = slices;
    if (slices) render.final_path[0] = render.temporary[0] = 0;   // A45: no save name; the slices take theirs
    const u32 n = bounce.from_preview ? studio.preview_frames : tape_frames(e.end - e.start, e.stretch);
    // n source frames as the player's 44.1 kHz frames, in 32 bits (review: not scale_frames, which
    // caps at its second argument)
    const u32 out = n / rate * 44100u + n % rate * 44100u / rate;
    bounce.until_quiet = e.tail >= kTailFull;   // A47
    bounce.capped = print_capped = false;
    bounce.got = 0;
    u32 total = out + 44100u * (bounce.until_quiet ? kFullTailS : e.tail);
    if (total > kSaveMaxFrames) total = kSaveMaxFrames;
    bounce.out_frames = out < total ? out : total;
    bounce.serial = ++bounce_serials;
    bounce_name(bounce.result, sizeof bounce.result, bounce.serial, ".wav");
    bounce_name(bounce.failure, sizeof bounce.failure, bounce.serial, ".fail");
    bounce.total = total;
    bounce.on = bounce.preparing = true;
    bounce.found = bounce.failed = bounce.renamed = bounce.polling = false;
    bounce.t0 = bounce.poked = now_ms();
    bounce.deadline = bounce.t0 + 30000u;   // until it starts (bounce_prepare_done sets the real one)
    studio.mode = kSaving;
    char line[24];
    u32 m = 0;
    line[0] = 0;
    append_number(line, sizeof line, m, total / 44100u + 1u);
    // A47: a "full" tail runs to quiet, at most this long ("35 s max; sample stops" fits the card's 23)
    append(line, sizeof line, m, bounce.until_quiet ? " s max; sample stops" : " s; sample stops");
    set_card("printing the fx live", line, 0);
    log(__LINE__, "bounce %d: %s through FX I %d, FX II %d, %d frames with the tail%s%s", static_cast<int>(bounce.serial),
        studio.path, static_cast<int>(e.send1), static_cast<int>(e.send2), static_cast<int>(total),
        bounce.from_preview ? ", the retime's preview" : "", slices ? ", then cut into slices" : "");
    queue(bounce_prepare_job, bounce_prepare_done);
    return true;
}
// A45: the slice page's print, cut as A40 cuts its one pass: the page's cuts on the edit's length, the tail
// on the last slice, each slice's fades and channels. The render carries on as any slice save (load_kit).
u32 print_out_frames;
void print_split_first(u32) {
    render.in_buf = static_cast<opxy::i16*>(allocate(kInFrames * render.in_channels * 2));
    render.chunk = static_cast<unsigned char*>(allocate(kChunkFrames * render.in_channels * 2));
    if (!render.in_buf || !render.chunk) { fail("no memory"); return; }
    render.fill = 0;
    begin_slices(print_out_frames, render.in_total);
}
void start_print_split() {
    busy = 1;
    const Edit& e = studio.edit;
    render.target = kToSlices;
    render.change = studio.change;
    render.edit = e;
    copy_text(render.source, studio.path, sizeof render.source);
    render.track = studio.track;
    render.engine = studio.engine;
    render.slot = studio.slot;
    render.new_kit = slices_new_kit;   // A30
    render.kit_folder[0] = 0;
    render.channels = 2;               // P11 writes 44.1 kHz stereo
    render.rate = 44100;
    render.chans = e.chans;
    render.out_channels = render.chans == kKeep ? 2 : 1;
    render.touched = false;
    render.fi = render.fo = render.step_in = render.step_out = 0;
    render.done = render.written = render.writes = 0;
    render.length = bounce.total;
    render.counting = render.streaming = render.two_pass = render.segmented = false;
    render.quick = render.superseded = false;
    render.object = nullptr;
    render.file = nullptr;
    render.chunk = nullptr;
    render.failed = false;
    render.why = "";
    render.serial = 0;
    render.final_path[0] = render.temporary[0] = 0;
    render.started_ms = now_ms();
    render.from_preview = false;
    render.printed = true;
    render.split = render.splitting = true;
    render.in_file = nullptr;
    render.in_buf = nullptr;
    render.in_start = render.in_frames = render.in_file_end = 0;
    copy_text(render.in_path, bounce.result, sizeof render.in_path);
    render.in_total = bounce.total;
    render.in_channels = 2;
    render.in_segmented = false;
    print_out_frames = bounce.out_frames;
    studio.mode = kSaving;
    set_card("cutting slices", base_name(studio.path), 0);
    queue(print_split_first, after_render);
}
// Every frame while a bounce runs: poke the audio board, look for its file, finish or give up.
void follow_bounce(u32 t) {
    if (!bounce.on || bounce.preparing) return;
    if (t - bounce.poked >= 300u) {
        b3(152, static_cast<i32>(bounce.serial));   // P11: the audio board's writer runs on these
        bounce.poked = t;
        if (!bounce.polling && !busy && !bounce.found && !bounce.failed) {
            bounce.polling = true;
            queue(bounce_check_job, bounce_check_done);
        }
    }
    if (bounce.polling) return;
    if (bounce.failed) {
        end_bounce("print failed", "the audio board gave up");
        return;
    }
    if (bounce.found) {
        bounce.on = false;
        if (bounce.got && bounce.got <= bounce.total) {
            bounce.capped = bounce.until_quiet && bounce.got >= bounce.total;   // A47: never went quiet
            print_capped = bounce.capped;
            if (bounce.until_quiet)
                log(__LINE__, "bounce %d: full tail: %d frames after the edit%s", static_cast<int>(bounce.serial),
                    static_cast<int>(bounce.got > bounce.out_frames ? bounce.got - bounce.out_frames : 0),
                    bounce.capped ? ", cut at the cap: still ringing" : ", quiet");
            bounce.total = bounce.got;
            if (bounce.out_frames > bounce.total) bounce.out_frames = bounce.total;
        }
        if (!bounce.renamed) {
            set_card("print failed", "couldn't name the file", 3000);
            studio.mode = kOpen;
            return;
        }
        if (bounce.slices) {
            log(__LINE__, "bounce %d: printed in %d ms; cutting it into slices", static_cast<int>(bounce.serial),
                static_cast<int>(t - bounce.t0));
            start_print_split();   // A45: the render carries on as any slice save, then load_kit
            return;
        }
        files_written();
        log(__LINE__, "saved %s -> %s: %d frames, 2 ch, 44100 Hz (printed in real time through FX I %d, FX II %d, %d ms)",
            studio.path, render.final_path, static_cast<int>(bounce.total), static_cast<int>(studio.edit.send1),
            static_cast<int>(studio.edit.send2), static_cast<int>(t - bounce.t0));
        render.length = bounce.total;
        render.track = studio.track;
        render.engine = studio.engine;
        render.slot = studio.slot;
        print_dry = false;
        studio.mode = kOpen;
        load_saved();
        if (bounce.capped) set_card("tail cut at 30 s", "still ringing", 5000);   // A47
        if (studio.leave_after_save) close_studio(false);
        return;
    }
    if (static_cast<i32>(t - bounce.deadline) > 0) end_bounce("print failed", "nothing came back");
}
void start_save() {
    if (bounce.on) return;
    if (busy) {
        if (render.target == kToPreview && !studio.save_pending) {
            // the save renders the whole edit itself; the preview stops at its next job
            studio.save_pending = true;
            studio.mode = kSaving;
            set_card("saving", base_name(studio.path), 0);
            return;
        }
        if (!studio.save_pending) set_card("still working", "", 1500);
        slice_choice_made = false;
        return;
    }
    studio.save_pending = false;
    flush_live(true);
    studio.mode = kSaving;
    if (studio.page == kChop) {
        chop_cuts();
        if (!slice_choice_made) {
            // A27, A30: new kit, files, or (a drum sampler) on this kit
            studio.mode = kSliceChoice;
            set_card("save slices", studio.engine == kDrumSampler ? "new kit, files, kit" : "new kit or files", 0);
            return;
        }
        slice_choice_made = false;
        // A45: slices with a send print the whole edit live first, then are cut from the print
        if (wants_bounce(studio.edit) && can_print(studio.edit) && start_bounce(true)) return;
        if (wants_bounce(studio.edit))
            log(__LINE__, "bounce: the slices can't print (%s): saved dry",
                live_ok(studio.edit) ? "a file with no key" : "the retime's whole preview isn't ready or passes 20 s");
        set_card("saving slices", base_name(studio.path), 0);
        start_render(kToSlices);
        return;
    }
    if (wants_bounce(studio.edit)) {
        // A44: a beat retime prints too, through its whole preview played live
        if (can_print(studio.edit) && start_bounce()) return;
        log(__LINE__, "bounce: can't print (%s): saved dry",
            live_ok(studio.edit) ? "no path for the save" : "the retime's whole preview isn't ready or passes 20 s");
    }
    set_card("saving", base_name(studio.path), 0);
    start_render(kToSave);
}

// ------------------------------------------------------------ the controls
const u32 kFadeMax = 2000;
u32 move_fade(u32 ms, i32 detents) {
    i32 v = static_cast<i32>(ms);
    const i32 dir = detents < 0 ? -1 : 1;
    for (i32 n = detents < 0 ? -detents : detents; n > 0; --n) {
        const i32 at = dir < 0 ? v - 1 : v;
        v += dir * (at < 50 ? 1 : at < 200 ? 5 : 20);
        if (v < 0) v = 0;
    }
    return v > static_cast<i32>(kFadeMax) ? kFadeMax : static_cast<u32>(v);
}
u32 snap_zero(u32 f) {
    if (!studio.ready) return f;
    const opxy::SampleView& v = studio.view;
    for (i32 d = 0; d < 256; ++d)
        for (i32 s = -1; s <= 1; s += 2) {
            const i32 g = static_cast<i32>(f) + s * d;
            if (g < 1 || g >= static_cast<i32>(v.frames)) continue;
            const i32 a = v.data[(g - 1) * static_cast<i32>(v.channels)], b = v.data[g * static_cast<i32>(v.channels)];
            if ((a < 0) != (b < 0) || b == 0) return static_cast<u32>(g);
        }
    return f;
}
u32 marker_step() {
    u32 step = total_frames() / (studio.zoom_marker ? 8 : 1) / 468;
    if (!step) step = 1;
    return step * (studio.run < 4 ? 1 : studio.run < 12 ? 4 : 16);
}
u32 step_by(u32 v, i32 detents, u32 step, u32 low, u32 high) {
    i32 x = static_cast<i32>(v) + detents * static_cast<i32>(step);
    if (x < static_cast<i32>(low)) x = static_cast<i32>(low);
    if (x > static_cast<i32>(high)) x = static_cast<i32>(high);
    return static_cast<u32>(x);
}
// The tape stretch that plays a pitch: 100 / 2^(st/12), in percent.
u32 tape_stretch(i32 semitones) { return 6553600u / dsp::kSemitoneQ16[semitones + 24]; }
i32 tape_pitch(u32 stretch) {
    const u32 speed = 6553600u / stretch;
    i32 best = 0;
    u32 gap = 0xffffffffu;
    for (i32 st = -24; st <= 24; ++st) {
        const u32 q = dsp::kSemitoneQ16[st + 24], d = q > speed ? q - speed : speed - q;
        if (d < gap) { gap = d; best = st; }
    }
    return best;
}
// Fit: the stretch that makes the edit last 1, 2, 4 or 8 bars at the project tempo.
void apply_fit(Edit& e) {
    if (!e.fit || !studio.ready) return;
    const u32 bpm10 = host::project_bpm_x10();
    if (!bpm10) return;
    const u32 bars = 1u << (e.fit - 1);
    const u32 target = studio.view.rate * 2400u / bpm10 * bars;   // bars * 4 beats * 60 s * 10 / bpm10
    const u32 length = e.end - e.start;
    if (!length) return;
    e.stretch = target / (length / 100u + 1u);
    if (e.stretch < 25) e.stretch = 25;
    if (e.stretch > 400) e.stretch = 400;
}
void turn_trim(u32 encoder, i32 detents, bool shift, u32 t) {
    Edit& e = studio.edit;
    const i32 dir = detents < 0 ? -1 : 1;
    const u32 n = static_cast<u32>(detents < 0 ? -detents : detents);
    const u32 total = total_frames();
    const u32 min_len = total > 64 ? 64 : 1;
    (void)shift;   // no Shift row (no silence, dc or chans)
    if (studio.row == 0) {
        if (encoder <= 1) {
            const u32 step = marker_step() * n;
            u32& m = encoder == 0 ? e.start : e.end;
            if (dir < 0) m = m > step ? m - step : 0;
            else m = m + step < total ? m + step : total;
            if (e.snap) m = snap_zero(m);
            if (encoder == 0 && e.start + min_len > e.end) e.start = e.end > min_len ? e.end - min_len : 0;
            if (encoder == 1 && e.end < e.start + min_len) e.end = e.start + min_len < total ? e.start + min_len : total;
            studio.zoom_marker = encoder + 1;
            studio.zoom_until = t + 1000;
            apply_fit(e);
        } else if (encoder == 2) {
            e.fade_in = move_fade(e.fade_in, detents);
        } else {
            e.fade_out = move_fade(e.fade_out, detents);
        }
    } else {
        if (encoder == 0) {
            if (e.normal) {
                e.level = live_level();
                e.normal = 0;
            }
            e.level += dir * static_cast<i32>(n);
            if (e.level < kLevelMin) e.level = kLevelMin;
            if (e.level > kLevelMax) e.level = kLevelMax;
        } else if (encoder == 1) {
            e.normal = step_by(e.normal, detents, 1, 0, 4);
        } else if (encoder == 2) {
            e.reverse = dir > 0 ? 1 : 0;
        } else {
            e.snap = dir > 0 ? 1 : 0;
        }
    }
}
void turn_chop(u32 encoder, i32 detents) {
    Edit& e = studio.edit;
    if (studio.row == 0) {
        if (encoder == 0) e.slices = step_by(e.slices, detents, 1, 2, kMaxSlices);
        else if (encoder == 1) e.chop_mode = detents > 0 ? 1 : 0;
        else if (encoder == 2) e.sens = step_by(e.sens, detents, 5, 0, 100);
        else e.slice = step_by(e.slice, detents, 1, 0, studio.cut_count);
    } else {
        if (encoder == 0) e.chop_fade = step_by(e.chop_fade, detents, 1, 0, 50);
        else if (encoder == 1) e.chop_snap = detents > 0 ? 1 : 0;
    }
}
void turn_time(u32 encoder, i32 detents) {
    Edit& e = studio.edit;
    if (studio.row == 0) {
        if (encoder == 0) {
            // clockwise is faster: a shorter length
            e.stretch = step_by(e.stretch, -detents, studio.run < 6 ? 1 : 5, 25, 400);
            e.fit = 0;
        } else if (encoder == 1) {
            if (e.beat) {
                e.pitch = static_cast<i32>(step_by(static_cast<u32>(e.pitch + 24), detents, 1, 0, 48)) - 24;
            } else {
                e.stretch = tape_stretch(static_cast<i32>(step_by(static_cast<u32>(tape_pitch(e.stretch) + 24), detents,
                                                                  1, 0, 48)) - 24);
                if (e.stretch < 25) e.stretch = 25;
                if (e.stretch > 400) e.stretch = 400;
                e.fit = 0;
            }
        } else if (encoder == 2) {
            e.beat = detents > 0 ? 1 : 0;
            if (!e.beat) e.pitch = 0;
        } else {
            e.fit = step_by(e.fit, detents, 1, 0, 4);
            apply_fit(e);
        }
    } else if (encoder == 0) {
        e.grain = step_by(e.grain, detents, 5, 20, 120);
    }
}
// The sweep knob's place from e.sweep and back (its own inverse): the low-pass side mirrored.
i32 sweep_place(i32 v) { return v < 0 ? -(static_cast<i32>(dsp::kCutoffCount) + 1 + v) : v; }
void turn_color(u32 encoder, i32 detents, bool) {
    Edit& e = studio.edit;
    if (studio.row == 0) {
        if (encoder == 0) e.rate_index = step_by(e.rate_index, detents, 1, 0, 8);
        else if (encoder == 1) e.bits_index = step_by(e.bits_index, detents, 1, 0, 8);
        else if (encoder == 2) e.drive = step_by(e.drive, detents, 2, 0, 100);
        else {
            // the sweep: left low-pass, right high-pass, through off. Turning down lowers the
            // frequency: the knob's place p runs -48..48 and the low-pass side is mirrored,
            // so one step left of off is the low-pass at 12 kHz and the far left is 50 Hz
            // (e.sweep -48 and -1); the high-pass side rises from 50 Hz.
            const i32 limit = static_cast<i32>(dsp::kCutoffCount);
            const i32 p = sweep_place(e.sweep);
            i32 v = p + detents;
            if (p < 0 && v > 0) v = 0;   // stop at off on the way through
            if (p > 0 && v < 0) v = 0;
            e.sweep = sweep_place(v < -limit ? -limit : v > limit ? limit : v);
        }
    } else {
        if (encoder == 0) e.send1 = step_by(e.send1, detents, 2, 0, 100);
        else if (encoder == 1) e.send2 = step_by(e.send2, detents, 2, 0, 100);
        else if (encoder == 2) e.tail = step_by(e.tail, detents, 1, 0, kTailFull);   // A47: past 10 s, "full"
        else e.comp = step_by(e.comp, detents, 2, 0, 100);
    }
}
void turn(u32 encoder, i32 detents) {
    if (!detents) return;
    const bool shift = false;   // no page has a Shift row
    const i32 id = static_cast<i32>(studio.page * 16 + encoder + (shift ? 8 : studio.row * 4));
    const u32 t = now_ms();
    if (id != studio.gesture) {
        push_undo(kEditStep);
        studio.gesture = id;
        studio.run = 0;
    } else {
        studio.run = t - studio.turn_ms < 120 ? studio.run + 1 : 0;
    }
    studio.turn_ms = t;
    if (studio.page == kTrim) turn_trim(encoder, detents, shift, t);
    else if (studio.page == kChop) turn_chop(encoder, detents);
    else if (studio.page == kTime) turn_time(encoder, detents);
    else turn_color(encoder, detents, shift);
    touched();
}

// ------------------------------------------------------------------- text
i32 q(i32 px) { return px * draw::unit; }
i32 width_q10(const char* text, i32 px) {
    u32 sum = 0;
    for (const char* p = text; *p; ++p) {
        const u32 c = static_cast<unsigned char>(*p);
        sum += c >= 32 && c < 127 ? panel::kAdvance[c - 32] : 560u;
    }
    return static_cast<i32>(sum * static_cast<u32>(px) * 1024u / 1000u);
}
void signed_number(char* out, u32 size, u32& n, i32 v) {
    if (v > 0) append(out, size, n, "+");
    if (v < 0) {
        append(out, size, n, "-");
        v = -v;
    }
    append_number(out, size, n, static_cast<u32>(v));
}
// A number with two decimals from hundredths: 125 -> "1.25".
void hundredths(char* out, u32 size, u32& n, u32 v) {
    append_number(out, size, n, v / 100);
    const char d[4] = {'.', static_cast<char>('0' + v / 10 % 10), static_cast<char>('0' + v % 10), 0};
    append(out, size, n, d);
}
// A readout is 5 characters at most with its unit ("0.000s" doesn't fit): "1.23",
// "12.3", "123" (then "s").
void millis_as_seconds(char* out, u32 size, u32& n, u32 ms) {
    append_number(out, size, n, ms / 1000);
    if (ms >= 100000) return;
    const u32 frac = ms % 1000;
    char d[4] = {'.', static_cast<char>('0' + frac / 100), static_cast<char>('0' + frac / 10 % 10), 0};
    if (ms >= 10000) d[2] = 0;
    append(out, size, n, d);
}
// A fade: "980ms", then "1.02s".
void fade_text(char* out, u32 size, u32 ms) {
    u32 n = 0;
    out[0] = 0;
    if (ms < 1000) {
        append_number(out, size, n, ms);
        append(out, size, n, "ms");
    } else {
        millis_as_seconds(out, size, n, ms);
        append(out, size, n, "s");
    }
}
void seconds(char* out, u32 size, u32 frames, u32 rate) {
    u32 n = 0;
    out[0] = 0;
    if (!rate) {
        append(out, size, n, "-");
        return;
    }
    const u32 ms = frames / rate * 1000 + frames % rate * 1000 / rate;
    millis_as_seconds(out, size, n, ms);
}
// Hz as "850" or "1.2k".
void hertz(char* out, u32 size, u32& n, u32 hz) {
    if (hz < 1000) {
        append_number(out, size, n, hz);
        return;
    }
    append_number(out, size, n, hz / 1000);
    if (hz < 10000) {
        const char d[3] = {'.', static_cast<char>('0' + hz / 100 % 10), 0};
        append(out, size, n, d);
    }
    append(out, size, n, "k");
}

// ---------------------------------------------------------------- drawing
// The SDK's surface over the library (opxy/sample_studio.hpp), bound to this extension's artwork.
using Frame = host::Surface;
Frame output;
bool bound, paths_ready;
u32 drawn_key = 0xffffffffu;
u32 path_key = 1;
void blank() { host::surface_blank(); }
bool ready_object() {
    if (!bound) bound = host::surface_ready(panel::kIds, panel::kPaths, panel::kNodes, panel::kTexts, panel::kPalette);
    return bound;
}
// A closed outline of `columns` amplitudes (fills, strips, bars, the bracket).
void ribbon(u32 p, i32 x0, i32 x1, const i32* amp, u32 columns, u32 key) {
    draw::Path<panel::kPoints>& path = output.paths[p];
    const u32 half = panel::kPoints / 2;
    for (u32 k = 0; k < half; ++k) {
        u32 c = k * columns / half;
        if (c >= columns) c = columns - 1;
        const i32 x = x0 + (x1 - x0) * static_cast<i32>(k) / static_cast<i32>(half - 1);
        const i32 a = amp[c];
        path.points[k] = draw::Point{x, -a};
        path.points[panel::kPoints - 1 - k] = draw::Point{x, a};
    }
    path.key = key;
}
// A waveform as two edges (no seams, as three equally spaced dents would show): its
// top on node `top` and its bottom on `top + 8` (dim0..3 -> dimlo0..3, lit0..3 -> litlo0..3), each a
// ribbon kEdge thick toward the middle (half a pixel: its closing edges stay over the device's 1/4 px
// sliver limit). No outline crosses the waveform, so the quarters meet without a seam or a pinch.
const i32 kEdge = 512;
static_assert(panel::N_DIMLO0 == panel::N_DIM0 + 8 && panel::N_LITLO0 == panel::N_LIT0 + 8, "edges 8 apart");
void edge(u32 p, i32 x0, i32 x1, const i32* amp, u32 columns, u32 key, i32 sign) {
    draw::Path<panel::kPoints>& path = output.paths[p];
    const u32 half = panel::kPoints / 2;
    for (u32 k = 0; k < half; ++k) {
        u32 c = k * columns / half;
        if (c >= columns) c = columns - 1;
        const i32 x = x0 + (x1 - x0) * static_cast<i32>(k) / static_cast<i32>(half - 1);
        path.points[k] = draw::Point{x, sign * amp[c]};
        path.points[panel::kPoints - 1 - k] = draw::Point{x, sign * (amp[c] - kEdge)};
    }
    path.key = key;
}
void wave(u32 top, i32 x0, i32 x1, const i32* amp, u32 columns, u32 key) {
    edge(top, x0, x1, amp, columns, key, -1);
    edge(top + 8, x0, x1, amp, columns, key, 1);
}
void show_wave(u32 top, i32 x, i32 y) {
    draw::place(output, top, x, y, draw::unit, 0, draw::keep);
    draw::place(output, top + 8, x, y, draw::unit, 0, draw::keep);
}
u32 fill_key = 0x40000001u;   // keys for the progress fill, apart from path_key's range
u32 next_fill_key() {
    if (++fill_key < 0x40000001u) fill_key = 0x40000001u;
    return fill_key;
}
void reset_frame() {
    for (u32 i = 0; i < panel::kNodes; ++i) draw::hide(output, i);
    for (u32 t = 0; t < panel::kTexts; ++t)
        draw::text(output, t, draw::hidden_x * draw::unit, draw::hidden_y * draw::unit, draw::unit, " ");
    for (u32 i = 0; i < 4; ++i) output.header[i][0] = 0;
    if (!paths_ready) {
        const i32 amp[1] = {1024};
        for (u32 p = 0; p < panel::kPaths; ++p) ribbon(p, -20 * 1024, 20 * 1024, amp, 1, path_key);
        paths_ready = true;
        // The placeholders took this key: the first waveform needs another, or the renderer keeps them
        // (blank until the first encoder turn).
        ++path_key;
    }
}
void show(u32 node, i32 x, i32 y, i32 scale = draw::unit) { draw::place(output, node, x, y, scale, 0, draw::keep); }
void say(u32 slot, i32 x, i32 y, const char* text) { draw::text(output, slot, x, y, draw::unit, text); }
const i32 kKeyX[4] = {30, 171, 310, 446};
i32 label_x(u32 i, const char* text) {
    const i32 w = width_q10(text, 20);
    i32 x = q(kKeyX[i]) - w / 2;
    if (x < q(6)) x = q(6);
    if (x > q(474) - w) x = q(474) - w;
    return x;
}
void m_label(u32 i, const char* text) { say(panel::T_M0 + i, label_x(i, text), q(215), text); }

const i32 kStageMid = 124, kStageHalf = 50, kX0 = 6, kWidth = 468;
struct View { u32 v0, v1; };
View source_view() {
    const u32 total = total_frames();
    if (!studio.zoom_marker) return View{0, total};
    u32 span = total / 8;
    if (span < 256) return View{0, total};
    const u32 at = studio.zoom_marker == 1 ? studio.edit.start : studio.edit.end;
    u32 v0 = at > span / 2 ? at - span / 2 : 0;
    if (v0 + span > total) v0 = total - span;
    return View{v0, v0 + span};
}
i32 x_of(const View& v, u32 f) {
    const u32 span = v.v1 - v.v0;
    if (!span || f <= v.v0) return q(kX0);
    if (f >= v.v1) return q(kX0 + kWidth);
    const u32 d = f - v.v0;
    return q(kX0) + static_cast<i32>(d * kWidth / span * 1024u + d * kWidth % span * 1024u / span);
}
// A46: reversing flips the waveform rather than running the playhead in reverse. On the trim
// page a reversed edit shows its source mirrored, its end at the left, so the playhead runs left to right.
bool mirrored() { return studio.page == kTrim && studio.edit.reverse; }
i32 x_shown(const View& v, u32 f) { return mirrored() ? q(kX0) + q(kX0 + kWidth) - x_of(v, f) : x_of(v, f); }
u32 fade_at(u32 f) {
    const Edit& e = studio.edit;
    if (f < e.start || f >= e.end) return 1024;
    const u32 n = e.end - e.start;
    const u32 i = e.reverse ? e.end - 1 - f : f - e.start;
    const u32 rate = studio.ready ? studio.view.rate : 44100;
    const u32 fi = e.fade_in * rate / 1000, fo = e.fade_out * rate / 1000;
    u32 g = 1024;
    if (fi && i < fi) g = i * 1024 / fi;
    if (fo && n - 1 - i < fo) {
        const u32 h = (n - 1 - i) * 1024 / fo;
        if (h < g) g = h;
    }
    return g;
}
i32 amp_q10(u32 a, bool gain, u32 f_mid, bool fades) {
    if (gain) {
        u32 scaled = (a * (shown_gain() >> 16)) >> 12;
        if (scaled > 32767) scaled = 32767;
        a = scaled;
    }
    if (fades) a = a * fade_at(f_mid) / 1024;
    const i32 px = static_cast<i32>(a * static_cast<u32>(kStageHalf) * 1024u / 32768u);
    return px < 1024 ? 1024 : px;
}
bool stage_redraw() { return studio.generation * 8 + studio.zoom_marker * 2 + studio.page != drawn_key; }
void stage_done() {
    drawn_key = studio.generation * 8 + studio.zoom_marker * 2 + studio.page;
    if (!waveform_said) {
        waveform_said = true;
        log(__LINE__, "waveform drawn: key %d, generation %d, peak %d", static_cast<int>(path_key),
            static_cast<int>(studio.generation), static_cast<int>(studio.peak));
    }
    if (++path_key >= 0x40000000u) path_key = 2;
}
// Trim: the whole source, the part between the markers lit.
// The output span (A20): output pages draw one waveform across a span that stays where it
// was fitted, so a new length shows at once in proportion (shorter: narrower); when its render
// lands the span eases to the new length over kFitMs (the waveform grows to the width) while
// the green flash fades. A length longer than the span fits at once.
const u32 kFitMs = 320, kFlashStepMs = 110, kFlashSteps = 4;
u32 axis_now(u32 t) {
    if (!axis_moving) return axis_frames;
    const u32 dt = t - axis_t0;
    if (dt >= kFitMs) {
        axis_moving = false;
        axis_frames = axis_to;
        return axis_frames;
    }
    const u32 x = dt * 1024u / kFitMs, eased = 1024u - ((1024u - x) * (1024u - x) >> 10);   // ease out
    const u32 d = axis_from > axis_to ? axis_from - axis_to : axis_to - axis_from;
    const u32 moved = scale_frames(eased, d, 1024);
    return axis_from > axis_to ? axis_from - moved : axis_from + moved;
}
View output_view() {
    const u32 n = output_length();
    if (!axis_frames) axis_frames = n;
    u32 total = axis_now(now_ms());
    if (!axis_moving && total < n) total = axis_frames = n;
    return View{0, total ? total : 1};
}
// The previews before the one now shown, removed (A11); not while one of them may be streaming.
void remove_passed() {
    // all but the one before the shown preview, which the audio board may still be reading (A19)
    if (studio.shown_serial <= studio.serial_first + 1) return;
    passed.track = studio.track;
    passed.from = studio.serial_first;
    passed.to = studio.shown_serial - 1;
    studio.serial_first = studio.shown_serial - 1;
    queue(passed_job, 0);
}
// A render landed: fit the span to it and flash.
void landed(u32 frames) {
    const u32 t = now_ms();
    axis_from = axis_now(t);
    axis_to = frames;
    axis_t0 = t;
    axis_moving = axis_from != axis_to && axis_frames;
    if (!axis_frames) axis_frames = frames;
    flash_t0 = t;
    flashing = true;
    ++studio.generation;
}
// The flash's palette step 1..4 now, or 0 once it has faded.
u32 flash_shade(u32 t) {
    if (!flashing) return 0;
    const u32 step = (t - flash_t0) / kFlashStepMs + 1;
    if (step > kFlashSteps) {
        flashing = false;
        return 0;
    }
    return step;
}
// How much of the output the running render has made, as output frames of `n` (red fill),
// or false when no render of the current edit is running.
bool rendered_so_far(u32 n, u32& covered) {
    if (!busy || render.target == kToSlices) return false;
    if (render.target == kToPreview && render.change != studio.change) return false;
    if (render.quick) {
        // the first part is the output's first frames (A17)
        const u32 step = n / 128u + 1u;
        covered = (render.done < n ? render.done : n) / step * step;
        return true;
    }
    u32 q16;
    const u32 length = render.length ? render.length : 1;
    if (render.streaming) {
        q16 = render.done >= render.estimate ? 65536u : scale_frames(render.done, 65536u, render.estimate);
    } else if (render.two_pass && render.counting) {
        q16 = scale_frames(render.length < n ? render.length : n, 32768u, n ? n : 1);
    } else if (render.two_pass) {
        q16 = 32768u + scale_frames(render.done < length ? render.done : length, 32768u, length);
    } else {
        q16 = scale_frames(render.done < length ? render.done : length, 65536u, length);
    }
    if (q16 > 65536u) q16 = 65536u;
    if (quick_shown() && studio.preview_frames && n) {
        // the bar goes on from where the first part left it, not from 0
        const u32 base = studio.preview_frames >= n ? 65536u : scale_frames(studio.preview_frames, 65536u, n);
        q16 = base + scale_frames(q16, 65536u - base, 65536u);
    }
    const u32 step = n / 128u + 1u;   // the fill moves in 1/128 steps: one outline reshaped at a time
    covered = scale_frames(q16, n, 65536u) / step * step;
    return true;
}
// One quarter's red: the part of [f0, f1) up to `covered`, over x0..x1 (relative to its centre).
u32 red_drawn[4];
template <class Amp>
void red_quarter(u32 qd, i32 cx, i32 x0, i32 x1, u32 f0, u32 f1, u32 covered, bool redraw, Amp amp_of) {
    if (covered <= f0 || f1 <= f0) {
        red_drawn[qd] = 0;
        return;
    }
    const u32 end = covered < f1 ? covered : f1;
    if (redraw || red_drawn[qd] != end) {
        i32 amp[32];
        for (u32 c = 0; c < 32; ++c) amp[c] = amp_of(f0 + (end - f0) * c / 32, f0 + (end - f0) * (c + 1) / 32);
        const i32 xe = x0 + static_cast<i32>(scale_frames(end - f0, static_cast<u32>(x1 - x0), f1 - f0));
        ribbon(panel::N_RED0 + qd, x0, xe > x0 + 1024 ? xe : x0 + 1024, amp, 32, next_fill_key());
        red_drawn[qd] = end;
    }
    show(panel::N_RED0 + qd, cx, q(kStageMid));
}
// The progress bar and the finish flash fill the stage's height behind the waveform:
// a full-height red progress bar in the background.
const i32 kBarAmp = 50 * 1024;
i32 bar_amp(u32, u32) { return kBarAmp; }
void green_quarter(u32 qd, i32 cx, u32 shade) {
    if (shade) draw::place(output, panel::N_GREEN0 + qd, cx, q(kStageMid), draw::unit, shade, draw::fill_and_stroke);
}
// A marker on a zero crossing of the first channel: 0, or a change of sign from the frame before.
bool at_zero(u32 f) {
    if (!studio.ready || f >= studio.view.frames) return false;
    const u32 ch = studio.view.channels;
    const i32 x = studio.view.data[f * ch];
    if (!x || !f) return !x;
    const i32 before = studio.view.data[(f - 1) * ch];
    return (before < 0) != (x < 0);
}
void trim_stage() {
    const View v = source_view();
    const bool redraw = stage_redraw();
    const u32 span = v.v1 - v.v0;
    const Edit& e = studio.edit;
    const u32 t = now_ms();
    const u32 shade = flash_shade(t);
    const bool mirror = mirrored();   // A46: a quarter's frames from the right, each drawn end first
    const u32 n_out = output_length(), src = e.end - e.start;
    u32 covered = 0, src_covered = 0;
    const bool rendering = rendered_so_far(n_out, covered);
    if (rendering) src_covered = scale_frames(covered, src, n_out ? n_out : 1);
    else for (u32 qd = 0; qd < 4; ++qd) red_drawn[qd] = 0;
    for (u32 qd = 0; qd < 4; ++qd) {
        const i32 cx = q(kX0) + q(kWidth) * static_cast<i32>(2 * qd + 1) / 8;
        const u32 shown = mirror ? 3 - qd : qd;
        const u32 f0 = v.v0 + span * shown / 4, f1 = v.v0 + span * (shown + 1) / 4;
        if (redraw) {
            i32 amp[32];
            for (u32 c = 0; c < 32; ++c) {
                const u32 k = mirror ? 31 - c : c;
                amp[c] = amp_q10(env_max(f0 + (f1 - f0) * k / 32, f0 + (f1 - f0) * (k + 1) / 32), false, 0, false);
            }
            wave(panel::N_DIM0 + qd, -q(kWidth) / 8, q(kWidth) / 8, amp, 32, path_key);
        }
        show_wave(panel::N_DIM0 + qd, cx, q(kStageMid));
        const u32 l0 = f0 > studio.edit.start ? f0 : studio.edit.start;
        const u32 l1 = f1 < studio.edit.end ? f1 : studio.edit.end;
        if (l1 <= l0) continue;
        const i32 x0 = x_shown(v, mirror ? l1 : l0) - cx, x1 = x_shown(v, mirror ? l0 : l1) - cx;
        if (x1 - x0 < q(8)) continue;
        if (redraw) {
            i32 amp[32];
            for (u32 c = 0; c < 32; ++c) {
                const u32 k = mirror ? 31 - c : c;
                const u32 a0 = l0 + (l1 - l0) * k / 32, a1 = l0 + (l1 - l0) * (k + 1) / 32;
                amp[c] = amp_q10(env_max(a0, a1), true, (a0 + a1) / 2, true);
            }
            wave(panel::N_LIT0 + qd, x0, x1, amp, 32, path_key);
            const i32 bar[1] = {kBarAmp};
            ribbon(panel::N_GREEN0 + qd, x0, x1, bar, 1, path_key);
        }
        if (rendering) {
            // the render's progress over the edit, in the source's frames (reversed: from the end)
            const u32 c0 = e.reverse ? e.end - src_covered : e.start, c1 = e.reverse ? e.end : e.start + src_covered;
            const u32 r0 = l0 > c0 ? l0 : c0, r1 = l1 < c1 ? l1 : c1;
            if (r1 > r0) {
                const i32 rx0 = x_shown(v, mirror ? r1 : r0) - cx, rx1 = x_shown(v, mirror ? r0 : r1) - cx;
                red_quarter(qd, cx, rx0, rx1 > rx0 + 1024 ? rx1 : rx0 + 1024, r0, r1, r1, redraw, bar_amp);
            } else {
                red_drawn[qd] = 0;
            }
        }
        green_quarter(qd, cx, shade);
        show_wave(panel::N_LIT0 + qd, cx, q(kStageMid));
    }
    if (e.snap && at_zero(e.start) && e.start >= v.v0 && e.start <= v.v1) show(panel::N_ZERO0, x_shown(v, e.start), q(kStageMid));
    if (e.snap && at_zero(e.end) && e.end >= v.v0 && e.end <= v.v1) show(panel::N_ZERO1, x_shown(v, e.end), q(kStageMid));
    // the markers in playing order: mirrored, the left one is the edit's end, where a reversed play begins
    const u32 first = mirror ? e.end : e.start, last = mirror ? e.start : e.end;
    if (first >= v.v0 && first <= v.v1) show(panel::N_START, x_shown(v, first), q(kStageMid));
    if (last >= v.v0 && last <= v.v1) show(panel::N_END, x_shown(v, last), q(kStageMid));
    if (studio.zoom_marker && span < total_frames()) {
        const u32 total = total_frames();
        for (u32 s = 0; s < 2; ++s) {
            if (redraw) {
                i32 amp[32];
                for (u32 c = 0; c < 32; ++c) {
                    const u32 k = mirror ? 63 - (s * 32 + c) : s * 32 + c;
                    const i32 a = static_cast<i32>(env_max(total / 64 * k, total / 64 * (k + 1)) * 2048u / 32768u);
                    amp[c] = a < 384 ? 384 : a;
                }
                ribbon(panel::N_STRIP0 + s, -q(kWidth) / 8, q(kWidth) / 8, amp, 32, path_key);
            }
            show(panel::N_STRIP0 + s, q(kX0) + q(kWidth) * static_cast<i32>(2 * s + 1) / 4, q(60), 2 * draw::unit);
        }
        const View whole = {0, total};
        const i32 b0 = x_shown(whole, mirror ? v.v1 : v.v0);
        i32 b1 = x_shown(whole, mirror ? v.v0 : v.v1);
        if (b1 - b0 < q(8)) b1 = b0 + q(8);
        if (redraw) {
            const i32 amp[1] = {q(6)};
            ribbon(panel::N_BRACKET, -(b1 - b0) / 2, (b1 - b0) / 2, amp, 1, path_key);
        }
        show(panel::N_BRACKET, (b0 + b1) / 2, q(60));
    }
    if (redraw) stage_done();
}
// Chop, time, color: the output, lit; on chop, the cuts and the auditioned slice.
void output_stage() {
    const u32 n = output_length();
    if (!n) return;
    const View v = output_view();
    const u32 t = now_ms();
    const bool redraw = stage_redraw() || axis_moving;
    const i32 lit = x_of(v, n) - q(kX0);
    const bool rendered = studio.out_valid && studio.rendered == studio.change;
    const u32 shade = flash_shade(t);
    u32 covered = 0;
    const bool rendering = rendered_so_far(n, covered);
    if (!rendering) for (u32 qd = 0; qd < 4; ++qd) red_drawn[qd] = 0;
    auto amp_of = [rendered](u32 a, u32 b) { return amp_q10(out_amp(a, b), !rendered, 0, false); };
    for (u32 qd = 0; qd < 4; ++qd) {
        if (lit < q(32)) break;   // too short to draw in four parts: the end marker shows it
        const u32 f0 = n / 4 * qd, f1 = qd == 3 ? n : n / 4 * (qd + 1);
        const i32 cx = q(kX0) + lit * static_cast<i32>(2 * qd + 1) / 8;
        if (redraw) {
            i32 amp[32];
            for (u32 c = 0; c < 32; ++c) amp[c] = amp_of(f0 + (f1 - f0) * c / 32, f0 + (f1 - f0) * (c + 1) / 32);
            wave(panel::N_LIT0 + qd, -lit / 8, lit / 8, amp, 32, path_key);
            const i32 bar[1] = {kBarAmp};
            ribbon(panel::N_GREEN0 + qd, -lit / 8, lit / 8, bar, 1, path_key);
        }
        if (rendering) red_quarter(qd, cx, -lit / 8, lit / 8, f0, f1, covered, redraw, bar_amp);
        green_quarter(qd, cx, shade);
        show_wave(panel::N_LIT0 + qd, cx, q(kStageMid));
    }
    if (studio.page != kChop) {
        show(panel::N_START, q(kX0), q(kStageMid));
        show(panel::N_END, x_of(v, n), q(kStageMid));
    }
    if (studio.page == kChop) {
        // the slice lines: the only lines over the waveform
        for (u32 k = 1; k < studio.cut_count && k <= 23; ++k)
            show(panel::N_CUT0 + k - 1, x_of(v, studio.cuts[k]), q(kStageMid));
        if (studio.edit.slice && studio.edit.slice <= studio.cut_count) {
            show(panel::N_START, x_of(v, studio.cuts[studio.edit.slice - 1]), q(kStageMid));
            show(panel::N_END, x_of(v, studio.cuts[studio.edit.slice]), q(kStageMid));
        }
    }
    if (redraw) stage_done();
}
// ---------------------------------------------------------------- the player (A19)
struct Play {
    bool on, original;
    u32 serial, start, end, rate, t0, sent;
    u32 mode, which;    // 1: the original by its hash; 2: preview `which` by its serial
    bool reverse, release;
    bool growing;       // A23: the preview is still being written
    bool by_note, held_seen;   // A22: started by a note (a gate), and its note seen held since
    bool live;                 // A24: the original through the player's chain
    u32 live_change;           // A24: the edit last sent while it plays
    u32 live_a, live_b, out_offset;
    bool live_slice;
    bool live_preview;          // A32: the live chain over the retime preview (mode 4)
    i32 level;
    i32 cents;                 // A48: the play's transposition (a note's against the key's root)
} play;
i32 next_cents;   // A48: the transposition the next play takes: a note's, 0 for any other play (a print's too)
void b3(u32 id, i32 value) { host::send(id - 138, value); }   // IDs 138..152: the player's channels 0..14
void player_send() {
    const u32 flags = (play.on ? 1u : 0u) | (play.reverse ? 4u : 0u) | (play.release ? 8u : 0u) |
                      (play.growing ? 16u : 0u);
    b3(138, static_cast<i32>((play.serial << 8) | flags));
    b3(139, static_cast<i32>(play.mode | (studio.track << 8) | ((static_cast<u32>(play.cents) & 0xffffu) << 16)));
    b3(140, static_cast<i32>(play.which));
    b3(141, static_cast<i32>(play.start));
    b3(142, static_cast<i32>(play.end));
    b3(143, play.level);
    b3(144, static_cast<i32>(play.serial));
    play.sent = now_ms();
    play.release = false;
}
u32 next_serial() {
    play.serial = (play.serial + 1) & 0x7fffffu;
    if (!play.serial) play.serial = 1;
    return play.serial;
}
void send_grow(u32 serial, u32 frames);
// `segmented`: the preview's frames on disk in all, when it is segment files (A23), else 0.
void play_file(const char* path, u32 start, u32 end, bool reverse, i32 level, bool original, u32 which = 0,
               bool growing = false, u32 segmented = 0) {
    if (end <= start || !studio.ready) return;
    next_serial();
    play.on = true;
    play.original = original;
    play.growing = growing;
    play.by_note = play.held_seen = false;
    play.live = false;
    play.live_preview = false;
    play.mode = original ? 1 : 2;
    play.which = original ? host::sample_key(path) : which ? which : studio.shown_serial;
    play.start = start;
    play.end = end;
    play.reverse = original && reverse;   // a render is already in playing order
    play.level = level;
    play.cents = next_cents;   // A48: the playhead moves at the transposed rate
    play.rate = dsp::mul_q16(studio.view.rate ? studio.view.rate : 44100, dsp::transpose_q16(play.cents));
    play.t0 = now_ms();
    play.live_change = live_rev;
    send_sends();                                      // A25
    if (segmented) send_grow(play.which, segmented);   // A23: its count before it is named
    player_send();
    static u32 logged_ms;
    if (play.t0 - logged_ms >= 300u) {   // notes from the sequencer: at most one line per 0.3 s
        logged_ms = play.t0;
        log(__LINE__, "play %s %d..%d%s, level %d%s", path, static_cast<int>(start), static_cast<int>(end),
            play.reverse ? " reversed" : "", static_cast<int>(level), play.growing ? ", still rendering" : "");
    }
}
void stop_play() {
    studio.compare = false;
    if (!play.on) return;
    play.on = false;
    play.live = false;
    next_serial();
    player_send();   // A38: a growing preview's stop keeps kGrowing; without it the audio board took it as landed
    play.growing = false;
}
// A preview landed: the audio board loads it now, so a press plays it at once. Under the
// playing command's serial (a new one would stop what plays); the next resend restores its fields.
void load_preview() {
    const u32 mode = play.mode, which = play.which;
    const bool on = play.on, growing = play.growing;
    play.growing = false;
    if (studio.shown_segmented) send_grow(studio.shown_serial, studio.preview_frames);   // A23: its last count
    if (!on) next_serial();
    play.mode = 2;
    play.which = studio.shown_serial;
    player_send();
    if (on) {
        play.mode = mode;
        play.which = which;
        play.growing = growing;
    }
}
// A23: a growing preview that plays has landed: it carries on with the real length.
void growing_landed(u32 serial, u32 length) {
    if (!play.on || !play.growing || play.which != serial) return;
    play.growing = false;   // A23
    play.end = length;
}
// A23: the frames of the running preview render now on the card.
void send_grow(u32 serial, u32 frames) {
    b3(146, static_cast<i32>((serial & 0xffffffu) | (studio.track << 24)));
    b3(147, static_cast<i32>(frames));
}
// A23: a preview render of the edit as it stands, with frames on the card.
bool growing_now() {
    return busy && render.target == kToPreview && !render.quick && !render.failed && render.change == studio.change &&
           render.segments && render.serial;
}
// The studio closes: the audio board lets its preview buffer go.
void release_player() {
    play.on = false;
    play.release = true;
    play.mode = 0;
    play.which = 0;
    next_serial();
    player_send();
}
// A22: the track silent while the studio is open (track + 1), or sounding again (0).
u32 hold_sent_ms;
void send_hold(u32 value) {
    b3(145, static_cast<i32>(value));
    hold_sent_ms = now_ms();
}
// ---------------------------------------------------------------- the live chain (A24)
const u32 kLiveParams = 19;
u32 live_sent[kLiveParams];
u32 peak_from, peak_to, peak_found;   // the markers' peak, as the render measures it for normalise
u32 marker_peak(u32 a, u32 b) {
    if (peak_from == a && peak_to == b && peak_found) return peak_found - 1;
    u32 m = 0;
    const u32 ch = studio.view.channels;
    for (u32 f = a; f < b && f < studio.view.frames; ++f)
        for (u32 c = 0; c < ch; ++c) {
            const i32 v = studio.view.data[f * ch + c];
            const u32 x = v < 0 ? static_cast<u32>(-v) : static_cast<u32>(v);
            if (x > m) m = x;
        }
    peak_from = a;
    peak_to = b;
    peak_found = m + 1;
    return m;
}
// The chain's settings for source frames a..b: the whole edit, or a slice with the chop fade.
// A32: `preview`: frames a..b of the retime preview, already in playing order and at its length.
void live_values(u32* v, const Edit& e, u32 a, u32 b, bool slice, bool preview = false) {
    dsp::Settings s = settings_of(e);
    if (preview) {
        s.reverse = 0;
        s.stretch = 100;
    }
    const u32 gain = e.normal ? normalise_gain(marker_peak(e.start, e.end), kNormalTarget[e.normal - 1]) : level_gain(e.level);
    const u32 rate = studio.view.rate ? studio.view.rate : 44100u;
    const u32 n = tape_frames(b - a, s.stretch);
    u32 fi = slice ? rate * e.chop_fade / 1000u : e.fade_in ? rate * e.fade_in / 1000u : 0;
    u32 fo = slice ? fi : e.fade_out ? rate * e.fade_out / 1000u : 0;
    if (fi > n) fi = n;
    if (fo > n) fo = n;
    v[0] = a;
    v[1] = b;
    v[2] = s.reverse;
    v[3] = gain >> 16;
    v[4] = gain & 0xffffu;
    v[5] = s.dc;
    v[6] = s.stretch;
    v[7] = s.rate_index;
    v[8] = s.bits_index;
    v[9] = s.drive;
    v[10] = static_cast<u32>(s.sweep) & 0xffffffu;   // signed, 24 bits
    v[11] = s.comp;
    v[12] = e.send1;
    v[13] = e.send2;
    v[14] = 0;
    v[15] = n;
    v[16] = fi;
    v[17] = fo;
    v[18] = studio.view.channels > 1 ? e.chans : 0;
}
// Send what changed since the last set (all of it the first time), then commit.
void send_live(u32 a, u32 b, bool slice, bool preview = false) {
    u32 v[kLiveParams];
    live_values(v, studio.edit, a, b, slice, preview);
    for (u32 k = 0; k < kLiveParams; ++k) {
        // the gain's two halves go together: the low half completes the word
        const bool changed = !live_sent_valid || v[k] != live_sent[k] || (k == 3 && v[4] != live_sent[4]) ||
                             (k == 4 && v[3] != live_sent[3]);
        if (changed) b3(148, static_cast<i32>((k << 24) | (v[k] & 0xffffffu)));
        live_sent[k] = v[k];
    }
    live_sent_valid = true;
    sends_sent[0] = v[12];
    sends_sent[1] = v[13];
    sends_valid = true;
    b3(149, 1);
}
// Play the edit live: source frames a..b (a slice, from `offset` on the output's axis).
void play_live(u32 a, u32 b, bool slice, u32 offset) {
    if (b <= a || !studio.ready) return;
    send_live(a, b, slice);
    next_serial();
    play.on = true;
    play.original = false;
    play.growing = false;
    play.by_note = play.held_seen = false;
    play.live = true;
    play.live_preview = false;
    play.live_change = live_rev;
    play.live_a = a;
    play.live_b = b;
    play.live_slice = slice;
    play.out_offset = offset;
    play.mode = 3;
    play.which = host::sample_key(studio.path);
    play.start = 0;
    play.end = tape_frames(b - a, studio.edit.stretch);
    play.reverse = false;   // the chain reverses
    play.level = 0;
    play.cents = next_cents;   // A48: the playhead moves at the transposed rate
    play.rate = dsp::mul_q16(studio.view.rate ? studio.view.rate : 44100, dsp::transpose_q16(play.cents));
    play.t0 = now_ms();
    player_send();
    static u32 logged_ms;
    if (play.t0 - logged_ms >= 300u) {
        logged_ms = play.t0;
        log(__LINE__, "play live %d..%d%s, %d frames out", static_cast<int>(a), static_cast<int>(b), slice ? " (slice)" : "",
            static_cast<int>(play.end));
    }
}
// A32: the retime preview's frames a..b through the live chain (the audio board's mode 4: the chain on
// the loaded preview), so the colour, gain, fades and channels follow the knobs without a render.
void play_live_preview(u32 a, u32 b, bool slice) {
    if (b <= a || !studio.ready || !studio.shown_serial) return;
    send_live(a, b, slice, true);
    next_serial();
    play.on = true;
    play.original = false;
    play.growing = false;
    play.by_note = play.held_seen = false;
    play.live = true;
    play.live_preview = true;
    play.live_change = live_rev;
    play.live_a = a;
    play.live_b = b;
    play.live_slice = slice;
    play.out_offset = a;
    play.mode = 4;
    play.which = studio.shown_serial;
    play.start = 0;
    play.end = b - a;
    play.reverse = false;
    play.level = 0;
    play.cents = next_cents;   // A48: the playhead moves at the transposed rate
    play.rate = dsp::mul_q16(studio.view.rate ? studio.view.rate : 44100, dsp::transpose_q16(play.cents));
    play.t0 = now_ms();
    send_sends();
    if (studio.shown_segmented) send_grow(play.which, studio.preview_frames);   // A23: its count, as play_file
    player_send();
    static u32 logged_ms;
    if (play.t0 - logged_ms >= 300u) {
        logged_ms = play.t0;
        log(__LINE__, "play live over preview %d: %d..%d%s", static_cast<int>(play.which), static_cast<int>(a),
            static_cast<int>(b), slice ? " (slice)" : "");
    }
}
// A25: the send amounts, when they changed (the player applies them at once, to any play).
void send_sends() {
    const u32 v[2] = {studio.edit.send1, studio.edit.send2};
    for (u32 k = 0; k < 2; ++k) {
        if (sends_valid && sends_sent[k] == v[k]) continue;
        b3(148, static_cast<i32>(((12u + k) << 24) | v[k]));
        sends_sent[k] = v[k];
    }
    sends_valid = true;
}
// While it plays, a changed edit goes to the player at once: the chain's colour and gain (the rest
// next play), and the sends for any play.
void follow_live() {
    if (!play.on || play.live_change == live_rev) return;
    play.live_change = live_rev;
    if (play.live) send_live(play.live_a, play.live_b, play.live_slice, play.live_preview);
    else send_sends();
}
// Shift + M3: the edit as it stands.
// A36: slice k's source frames (cuts are output frames; a stretch makes the output longer than the source).
void slice_source(const Edit& e, u32 k, u32& a, u32& b) {
    const u32 src_len = e.end - e.start, n = output_length();
    u32 s0 = scale_frames(studio.cuts[k - 1], src_len, n), s1 = scale_frames(studio.cuts[k], src_len, n);
    if (s1 > src_len) s1 = src_len;
    if (s0 > s1) s0 = s1;
    a = e.reverse ? e.end - s1 : e.start + s0;
    b = e.reverse ? e.end - s0 : e.start + s1;
}
void play_edit() {
    const Edit& e = studio.edit;
    studio.compare = false;   // A26: play switches over from compare
    if (live_ok(e)) {
        // A24: the original through the chain, no render
        if (studio.page == kChop && e.slice && e.slice <= studio.cut_count) {
            u32 a = 0, b = 0;
            slice_source(e, e.slice, a, b);   // A36
            play_live(a, b, true, studio.cuts[e.slice - 1]);
        } else {
            play_live(e.start, e.end, false, 0);
        }
        return;
    }
    if (growing_now()) {
        // A23: what's on the card of the render under way, then more on the next press
        u32 n = render.streaming ? render.estimate : render.length;
        if (n > kMaxFrames) n = kMaxFrames;
        play_file(render.final_path, 0, n, false, 0, false, render.serial, true, render.synced);
        return;
    }
    if (studio.showing_preview && (studio.rendered == studio.change || quick_shown())) {
        u32 a = 0, b = studio.preview_frames;
        if (!quick_shown()) audition(a, b);
        if (b > studio.preview_frames) b = studio.preview_frames;
        if (!quick_shown() && !live_ok(e)) {
            // A32: the retime render, with everything else live over it
            play_live_preview(a, b, studio.page == kChop && e.slice && e.slice <= studio.cut_count);
            return;
        }
        play_file(studio.preview, a, b, false, 0, false, 0, false, studio.shown_segmented ? studio.preview_frames : 0);
    } else if (!needs_render(e) || !studio.showing_preview) {
        // the original between the markers (or a slice of it), its direction and level
        u32 a = e.start, b = e.end;
        if (studio.page == kChop && e.slice && e.slice <= studio.cut_count) {
            slice_source(e, e.slice, a, b);   // A36
        }
        play_file(studio.path, a, b, e.reverse != 0, live_level(), true);
    } else {
        // a render is due: the last preview meanwhile
        play_file(studio.preview, 0, studio.preview_frames, false, 0, false, 0, false,
                  studio.shown_segmented ? studio.preview_frames : 0);
    }
}
// Shift + M2 held: the original as the studio opened it.
void play_original() {
    const opxy::SampleRegion r = host::entry_region(studio.origin);
    const u32 frames = total_frames();
    u32 a = r.begin, b = r.end ? r.end : frames;
    if (b > frames) b = frames;
    if (a >= b) a = 0;
    play_file(studio.path, a, b, host::entry_reversed(studio.origin), host::entry_level(studio.origin), true);
}
// A22, every frame while the studio is open: hold the track, and play the edit on its notes.
u32 track_notes() { return host::note_serial(studio.track); }
bool track_held() {
    for (u32 n = 0; n < 128; ++n)
        if (host::note_held(studio.track, n)) return true;
    return false;
}
// A33: a pad triggers its own slice. On the slice page the track's
// first key (53, the kit's first pad, as New kit lays slices out) plays slice 1, the next slice 2, and
// so on; a key past the last slice, or a note too short to be seen held, plays the whole edit.
unsigned char notes_before[128];
u32 struck_note = 128, struck_ms;
// The note last pressed on the track within kTapMs (it may show a frame before or after the serial moves).
u32 note_struck(u32 t) {
    for (u32 n = 0; n < 128; ++n) {
        const unsigned char held = host::note_held(studio.track, n) ? 1 : 0;
        if (held && !notes_before[n]) {
            struck_note = n;
            struck_ms = t;
        }
        notes_before[n] = held;
    }
    return t - struck_ms <= kTapMs ? struck_note : 128;
}
// A48 A note plays the edit as the track would play it: on a sampler or multisampler, transposed
//    by the note against the key's root and fine (on a multisampler, the edited zone's key for every
//    note), within stock's two octaves either way; a drum track's pads (drum sampler, Drumline, a
//    synthesis kit) and the slice page's pads at their own pitch; a file on no key from its own root (A49).
//    Audition only: the play button, prints and saves stay at the file's own pitch (player.cpp P16).
i32 note_cents(u32 note) {
    if (note >= 128 || studio.track >= 8 || host::drum_track(studio.track)) return 0;
    const unsigned char* key = studio.on_key ? studio.key_entry : studio.file_only ? nullptr : studio.origin;
    i32 root_cents = studio.file_root;   // A49: a file on no key, from its own root
    if (key) {
        u32 root = host::entry_root(key);
        const i32 fine = host::entry_fine(key);
        if (!root && !fine) root = 60;   // A34: an empty key's 0, 0 is neutral
        root_cents = static_cast<i32>(root) * 100 - fine;
    }
    i32 cents = static_cast<i32>(note) * 100 - root_cents;
    if (cents > dsp::kMaxTransposeCents) cents = dsp::kMaxTransposeCents;
    if (cents < -dsp::kMaxTransposeCents) cents = -dsp::kMaxTransposeCents;
    return cents;
}
void follow_notes(u32 t) {
    if (studio.track >= 8) return;   // the auxiliary and FX tracks: no notes to follow
    if (t - hold_sent_ms >= kHoldResendMs) send_hold(studio.track + 1);
    const u32 serial = track_notes();
    const u32 struck = note_struck(t);
    if (serial != note_seen) {
        note_seen = serial;
        if (studio.mode == kOpen && studio.ready) {
            const u32 slice = struck >= kKitFirstKey ? struck - kKitFirstKey + 1 : 0;
            if (studio.page == kChop && struck < 128)
                log(__LINE__, "pad: note %d is pad %d of %d slices", static_cast<int>(struck), static_cast<int>(slice),
                    static_cast<int>(studio.cut_count));
            if (studio.page == kChop && slice && slice <= studio.cut_count) {
                const u32 kept = studio.edit.slice;
                studio.edit.slice = slice;
                play_edit();
                studio.edit.slice = kept;
            } else {
                next_cents = note_cents(struck);   // A48
                play_edit();
                next_cents = 0;
            }
            play.by_note = play.on;
        }
    }
    if (!play.on || !play.by_note) return;
    // the gate: released once no note of the track is held (after one was seen, or a tap)
    if (track_held()) play.held_seen = true;
    else if (play.held_seen || t - play.t0 > kTapMs) stop_play();
}
// Every frame: keep a playing command alive; stop showing it once it has run out.
void keep_playing(u32 t) {
    if (!play.on) return;
    const u32 moved = (t - play.t0) * (play.rate / 10u) / 100u;
    if (t - play.t0 > 600000u || moved >= play.end - play.start + play.rate / 2u) {
        stop_play();   // done (with half a second's slack): the stop also has the audio board log its figures
        return;
    }
    if (t - play.sent >= kPlayResendMs) player_send();
}
void playhead(u32 t) {
    if (!play.on || !studio.ready) return;
    const u32 moved = (t - play.t0) * (play.rate / 10u) / 100u;
    if (moved >= play.end - play.start) return;
    const u32 at = play.reverse ? play.end - 1 - moved : play.start + moved;
    const Edit& e = studio.edit;
    const u32 src = e.end - e.start;
    // the frame as an output frame and as a source frame
    u32 out_f, src_f;
    const u32 n = output_length();
    if (play.growing) {
        // a render under way: its own output frames, held where the card's frames end
        const u32 on_card = busy && render.serial == play.which ? render.synced : at;
        out_f = at < on_card ? at : on_card;
        const u32 o = scale_frames(out_f < n ? out_f : n, src, n ? n : 1);
        src_f = e.reverse ? e.end - (o < src ? o : src) : e.start + o;
    } else if (play.live) {
        out_f = play.out_offset + at;   // A24: the chain's output frames are the output's
        const u32 o = scale_frames(out_f < n ? out_f : n, src, n ? n : 1);
        src_f = e.reverse ? e.end - (o < src ? o : src) : e.start + o;
    } else if (!play.original) {
        out_f = quick_shown() ? at : scale_frames(at, n, studio.preview_frames ? studio.preview_frames : 1);
        const u32 o = scale_frames(out_f, src, n ? n : 1);
        src_f = e.reverse ? e.end - (o < src ? o : src) : e.start + o;
    } else {
        src_f = at;
        const u32 o = at < e.start ? 0 : at > e.end ? src : at - e.start;
        out_f = scale_frames(e.reverse ? src - o : o, n, src ? src : 1);
    }
    if (studio.page == kTrim) {
        const View v = source_view();
        if (src_f < v.v0 || src_f > v.v1) return;
        show(panel::N_PLAYHEAD, x_shown(v, src_f), q(kStageMid));   // A46: mirrored, left to right
    } else {
        show(panel::N_PLAYHEAD, x_of(output_view(), out_f), q(kStageMid));
    }
}
// The header: the SDK's two-row page header (opxy/page_header.hpp), as the synths' pages use
// it, so an encoder press moves the rows with the same eased slide. Its eight bar
// nodes are bar0..7 and its sixteen text slots the page's title, value, under_title and
// under_value slots 0..15, in that order. `page` is the bright row.
void studio_header(const char* const* top, const char* const* top_values, const char* const* under,
                   const char* const* under_values, u32 page) {
    const char* titles[8];
    char shown_values[8][draw::header_capacity];
    for (u32 i = 0; i < 4; ++i) {
        titles[i] = top[i];
        titles[4 + i] = under[i];
        copy_text(shown_values[i], top_values[i], draw::header_capacity);
        copy_text(shown_values[4 + i], under_values[i], draw::header_capacity);
    }
    draw::Input in = {};
    const u32 t = now_ms();
    const u32 dt = header_ms ? t - header_ms : 0;
    header_ms = t;
    in.elapsed = static_cast<i32>((dt > 1000u ? 1000u : dt) * 1024u / 1000u);   // Q10 seconds
    in.track = studio.track;
    for (u32 col = 0; col < 4; ++col) in.shown[col] = static_cast<unsigned char>(page ? 4 + col : col);
    draw::page_header(header_state, in, output, titles, shown_values, panel::N_BAR0);
}
char values[12][12];
void clear_values() {
    for (u32 i = 0; i < 12; ++i) values[i][0] = 0;
}
void trim_values() {
    const Edit& e = studio.edit;
    const u32 rate = studio.ready ? studio.view.rate : 44100;
    u32 n;
    // units on every readout, as stock's pages show them (7 characters fit a header value)
    seconds(values[0], 12, e.start, rate);
    n = 0; while (values[0][n]) ++n; append(values[0], 12, n, "s");
    seconds(values[1], 12, e.end, rate);
    n = 0; while (values[1][n]) ++n; append(values[1], 12, n, "s");
    fade_text(values[2], 12, e.fade_in);
    fade_text(values[3], 12, e.fade_out);
    n = 0;
    if (e.normal) append(values[4], 12, n, "auto");
    else { signed_number(values[4], 12, n, e.level); append(values[4], 12, n, "dB"); }
    n = 0;
    if (!e.normal) append(values[5], 12, n, "off");
    else { signed_number(values[5], 12, n, kNormalDb[e.normal - 1]); append(values[5], 12, n, "dB"); }
    copy_text(values[6], e.reverse ? "on" : "off", 12);
    copy_text(values[7], e.snap ? "zero" : "off", 12);
}
void chop_values() {
    const Edit& e = studio.edit;
    u32 n = 0;
    append_number(values[0], 12, n, e.chop_mode ? studio.cut_count : e.slices);
    copy_text(values[1], e.chop_mode ? "trans" : "even", 12);
    n = 0; append_number(values[2], 12, n, e.sens);
    n = 0;
    if (!e.slice) append(values[3], 12, n, "all");
    else append_number(values[3], 12, n, e.slice);
    n = 0; append_number(values[4], 12, n, e.chop_fade); append(values[4], 12, n, "ms");
    copy_text(values[5], e.chop_snap ? "zero" : "off", 12);
}
void time_values() {
    const Edit& e = studio.edit;
    u32 n = 0;
    // playback speed: x2.00 twice as fast and half as long; e.stretch is the length in percent
    append(values[0], 12, n, "x");
    hundredths(values[0], 12, n, (10000u + e.stretch / 2u) / e.stretch);
    n = 0;
    signed_number(values[1], 12, n, e.beat ? e.pitch : tape_pitch(e.stretch));
    append(values[1], 12, n, "st");
    copy_text(values[2], e.beat ? "beat" : "tape", 12);
    const char* fits[5] = {"off", "1 bar", "2 bar", "4 bar", "8 bar"};
    copy_text(values[3], fits[e.fit < 5 ? e.fit : 0], 12);
    n = 0; append_number(values[4], 12, n, e.grain); append(values[4], 12, n, "ms");
}
void color_values() {
    const Edit& e = studio.edit;
    u32 n = 0;
    if (!e.rate_index) append(values[0], 12, n, "off");
    else hertz(values[0], 12, n, dsp::kRateHz[e.rate_index]);
    n = 0;
    if (!e.bits_index) append(values[1], 12, n, "off");
    else { append_number(values[1], 12, n, dsp::kBits[e.bits_index]); append(values[1], 12, n, "bit"); }
    n = 0; append_number(values[2], 12, n, e.drive); append(values[2], 12, n, "%");
    n = 0;
    if (!e.sweep) {
        append(values[3], 12, n, "off");
    } else {
        append(values[3], 12, n, e.sweep < 0 ? "-" : "+");   // - low-pass, + high-pass
        hertz(values[3], 12, n, dsp::kCutoffHz[(e.sweep < 0 ? -e.sweep : e.sweep) - 1]);
    }
    n = 0; append_number(values[4], 12, n, e.send1); append(values[4], 12, n, "%");
    n = 0; append_number(values[5], 12, n, e.send2); append(values[5], 12, n, "%");
    n = 0;
    if (e.tail >= kTailFull) append(values[6], 12, n, "full");   // A47
    else { append_number(values[6], 12, n, e.tail); append(values[6], 12, n, "s"); }
    n = 0; append_number(values[7], 12, n, e.comp); append(values[7], 12, n, "%");
}
void file_line() {
    char left[24], right[24], cut[15];
    u32 n = 0;
    left[0] = right[0] = 0;
    copy_text(cut, base_name(studio.path), sizeof cut);
    append(left, sizeof left, n, cut);
    if (dirty()) append(left, sizeof left, n, " (edited)");
    say(panel::T_FILE_LEFT, q(6), q(50), left);
    n = 0;
    const Edit& e = studio.edit;
    // the render's state, in red (the alert slot's authored fill)
    const char* alert = nullptr;
    if (studio.ready) {
        if (studio.loading_preview) alert = "loading the render";
        else if (preview_render(e) && studio.rendered != studio.change)
            alert = quick_shown() ? "rendering the rest" : busy ? "rendering" : "about to render";
        else if (preview_render(e) && studio.failed_change == studio.change && studio.failures >= kRetries)
            alert = "render failed";
        else if (studio.preview_lost && studio.showing_preview) alert = "render not loaded";
    }
    if (alert) {
        say(panel::T_FILE_ALERT, q(474) - width_q10(alert, 10), q(50), alert);
        return;
    }
    if (!studio.ready) {
        append(right, sizeof right, n, studio.pcm_failed ? "waiting for the sample" : "loading");
    } else if (studio.cut_at_limit && preview_render(e)) {
        append(right, sizeof right, n, "cut at 20 s");
    } else {
        char s[12];
        seconds(s, sizeof s, output_length(), studio.view.rate);
        append(right, sizeof right, n, s);
        append(right, sizeof right, n, studio.view.channels > 1 ? " s  stereo  " : " s  mono  ");
        append_number(right, sizeof right, n, studio.view.rate / 1000);
        append(right, sizeof right, n, "k");
    }
    say(panel::T_FILE_RIGHT, q(474) - width_q10(right, 10), q(50), right);
}
void card() {
    if (!studio.card1[0]) return;
    show(panel::N_CARD, q(240), q(110));
    const i32 w1 = width_q10(studio.card1, 20);
    if (studio.card2[0]) {
        say(panel::T_CARD1, q(240) - w1 / 2, q(104), studio.card1);
        say(panel::T_CARD2, q(240) - width_q10(studio.card2, 14) / 2, q(126), studio.card2);
    } else {
        say(panel::T_CARD1, q(240) - w1 / 2, q(117), studio.card1);
    }
    if (studio.mode == kSaving && render.length && !render.counting) {
        show(panel::N_TRACK, q(240), q(137), 2 * draw::unit);
        const u32 done = render.done / (render.length / 128u + 1u);
        if (done >= 4 && done <= 128) {
            const i32 amp[1] = {1024};
            ribbon(panel::N_PROGRESS, -q(64), -q(64) + q(static_cast<i32>(done)), amp, 1, 0x40000000u + done);
            show(panel::N_PROGRESS, q(240), q(137), 2 * draw::unit);
        }
    }
}
const char* const kPageNames[4] = {"trim", "slice", "retime", "process"};   // M1..M4
void page() {
    reset_frame();
    show(panel::N_BG, 0, 0);
    clear_values();
    const bool shift = host::shift_held();
    const bool a = studio.row == 0;
    if (studio.page == kTrim) {
        trim_values();
        const char* row_a_t[4] = {"start", "end", "fade in", "fade out"};
        const char* row_a_v[4] = {values[0], values[1], values[2], values[3]};
        const char* row_b_t[4] = {"level", "normal", "reverse", "snap"};
        const char* row_b_v[4] = {values[4], values[5], values[6], values[7]};
        studio_header(row_a_t, row_a_v, row_b_t, row_b_v, studio.row);   // no Shift row
    } else {
        const char* t_a[4] = {"", "", "", ""};
        const char* t_b[4] = {"", "", "", ""};
        if (studio.page == kChop) {
            chop_values();
            const char* x[4] = {"slices", "mode", "sens", "slice"};
            const char* y[4] = {"fade", "snap", "", ""};
            for (u32 i = 0; i < 4; ++i) { t_a[i] = x[i]; t_b[i] = y[i]; }
        } else if (studio.page == kTime) {
            time_values();
            const char* x[4] = {"speed", "pitch", "mode", "fit"};
            const char* y[4] = {"grain", "", "", ""};
            for (u32 i = 0; i < 4; ++i) { t_a[i] = x[i]; t_b[i] = y[i]; }
        } else {
            color_values();
            const char* x[4] = {"rate", "bits", "drive", "filter"};
            const char* y[4] = {"fx i", "fx ii", "tail", "comp"};
            for (u32 i = 0; i < 4; ++i) { t_a[i] = x[i]; t_b[i] = y[i]; }
        }
        const char* v_a[4] = {values[0], values[1], values[2], values[3]};
        const char* v_b[4] = {values[4], values[5], values[6], values[7]};
        studio_header(t_a, v_a, t_b, v_b, studio.row);
    }
    file_line();
    if (studio.ready) {
        if (studio.page == kTrim) trim_stage();
        else output_stage();
        playhead(now_ms());
    }
    if (studio.mode == kLeaving) {
        m_label(0, "discard");
        m_label(1, "back");
        m_label(3, "save");
    } else if (studio.mode == kSliceChoice) {
        m_label(0, "back");
        m_label(1, "new kit");
        m_label(2, "files");
        if (studio.engine == kDrumSampler) m_label(3, "this kit");
    } else if (studio.mode == kOpen && shift) {
        m_label(0, "undo");
        m_label(1, play.on && studio.compare ? "stop" : "compare");   // A26: each latches
        m_label(2, play.on && !studio.compare ? "stop" : "play");
        m_label(3, studio.page == kChop ? "save slices" : "save");
    } else if (studio.mode == kOpen) {
        for (u32 i = 0; i < 4; ++i) {
            const i32 x = label_x(i, kPageNames[i]);
            if (i == studio.page) {
                show(panel::N_TAB0 + i, x - q(4), q(197));
                say(panel::T_TAB_TEXT, x, q(215), kPageNames[i]);
            } else {
                say(panel::T_M0 + i, x, q(215), kPageNames[i]);
            }
        }
    }
    if (studio.card_until && static_cast<i32>(now_ms() - studio.card_until) > 0 && studio.mode == kOpen) clear_card();
    card();
}
// The way in, over M1: "edit" on a sampler track (M1 opens) or while Shift is held; on a drum
// sampler or multisampler, where M1 is stock's previous key, nothing until Shift is held, so stock's
// arrow shows alone ("shift: edit" would sit over
// the arrow).
void label() {
    reset_frame();
    const u32 track = host::selected_track();
    const u32 ui = track < 8 ? host::sample_engine(track) : 0;
    const bool needs_shift = ui == kDrumSampler || ui == kMultisampler;   // M1 is stock's previous key there
    if (!needs_shift || host::shift_held()) m_label(0, "edit");
}
// A28: the label on every track while the library is on top and not renaming or deleting; on a
// track without a sample key, M1 then opens the highlighted file, or asks for one on a folder.
// The frame never asks stock's library tree for its highlight: building and freeing that string
// from the screen's frame crashes the library as it opens. Stock's own record of the last
// highlighted file isn't enough either: stock writes it only once the highlight moves.
bool label_wanted() { return guards_clear(); }
void take_snapshot() {
    u32 track, engine, slot;
    snapshot.valid = false;
    if (!selected(track, engine, slot)) return;
    const unsigned char* entry = region_entry(track, slot);
    if (!entry || frames_of(entry) <= 0) return;
    snapshot.track = track;
    snapshot.slot = slot;
    copy_bytes(snapshot.entry, entry, kEntryBytes);
    copy_text(snapshot.path, host::entry_path(entry), sizeof snapshot.path);
    snapshot.valid = true;
}
bool open_from_library(bool shift) {
    if (!guards_clear()) return false;
    u32 track, engine, slot;
    char path[kPathBytes];
    if (selected(track, engine, slot)) {
        if (!shift && engine != kSampler) return false;   // M1 is stock's previous key on drum and multi
        // A31: the highlighted file, if it isn't the key's own (the library puts it on the key
        // on a sampler, not on a drum sampler or multisampler)
        const unsigned char* entry = region_entry(track, slot);
        const char* own = entry ? host::entry_path(entry) : "";
        if (engine != kSampler && highlighted(path, sizeof path) && !same_text(path, own, kPathBytes))
            return open_file(track, path, engine, slot);
        open_studio(track, engine, slot);
        return true;
    }
    // A28: file mode, the browser's highlighted file
    track = host::selected_track();
    const u32 ui = track < 8 ? host::sample_engine(track) : 0;
    if (!shift && (ui == kDrumSampler || ui == kMultisampler)) return false;
    if (!highlighted(path, sizeof path)) { popup("highlight a sample first"); return false; }
    return open_file(track, path);
}
void leave() {
    if (dirty()) {
        studio.mode = kLeaving;
        set_card("save your edit?", "original stays as is", 0);
    } else {
        close_studio(false);
    }
}
bool owned(u32 key) { return (key >= kKeyM1 && key <= kKeyClick3) || key == kKeySample; }
// Every frame the studio is open: a render when one is due (A11).
void maybe_render(u32 t) {
    if (busy || studio.mode != kOpen || !studio.ready || studio.compare || studio.loading_preview) return;
    if (!preview_render(studio.edit) || studio.rendered == studio.change) return;   // A24: live edits never render
    if (t - studio.changed_ms < kDebounceMs) return;
    if (studio.failed_change == studio.change && studio.failures && static_cast<i32>(t - studio.retry_at_ms) < 0) return;
    // A17: a long output gets its first part on the key first
    const bool long_output = output_length() / (studio.view.rate ? studio.view.rate : 44100u) * 1000u >= kQuickMinMs;
    start_render(kToPreview, long_output && studio.quick_change != studio.change);
}
// Is the preview on the key in stock's cache yet? (A13)
u32 check_key, check_serial, check_frames, check_channels;
bool check_found;
CacheState check_state;
bool check_unwanted;
void check_job(u32) {
    const Borrowed b = borrow(check_key);
    const void* object = b.object;
    check_state = b.state;
    check_unwanted = b.unwanted;
    check_found = object != nullptr;
    if (!object) return;
    const opxy::SampleView view = host::sample_view(object);
    check_frames = view.frames;
    check_channels = view.channels;
    host::release_sample(check_key);
}
void check_done(u32) {
    studio.checking = false;
    if (studio.mode == kClosed || !studio.loading_preview || check_serial != studio.shown_serial) return;
    const u32 t = now_ms();
    const i32 waited = static_cast<i32>(t - studio.swapped_ms);
    if (check_found) {
        studio.loading_preview = false;
        if (studio.preview_streamed && check_frames != studio.preview_frames && !header_patch_failed) {
            header_patch_failed = true;   // A18: the rewrite didn't take; count from now on
            log(__LINE__, "preview %s: one-pass header not taken (cache %d, written %d): counting from now on",
                studio.preview, static_cast<int>(check_frames), static_cast<int>(studio.preview_frames));
        }
        log(__LINE__, "preview %s in the cache after %d ms: %d frames, %d ch, %s by stock%s", studio.preview, waited,
            static_cast<int>(check_frames), static_cast<int>(check_channels), check_unwanted ? "unwanted" : "wanted",
            check_frames == studio.preview_frames ? "" : " (not the written length: an older file?)");
        // the files the key has moved past
        passed.track = studio.track;
        passed.from = studio.serial_first;
        passed.to = studio.shown_serial;
        studio.serial_first = studio.shown_serial;
        if (passed.from < passed.to) queue(passed_job, 0);
        ++studio.generation;
        return;
    }
    if (!studio.resent && waited >= static_cast<i32>(kResendMs)) {
        studio.resent = true;
        log(__LINE__, "preview %s after %d ms: %s%s; region table sent again", studio.preview, waited,
            cache_words(check_state), check_state != kNotCached && check_unwanted ? " (unwanted by stock)" : "");
        send_regions(studio.track, true);
    }
    if (waited >= static_cast<i32>(kGiveUpMs)) {
        studio.loading_preview = false;
        studio.preview_lost = true;
        log(__LINE__, "preview %s after %d ms: %s%s; stopped waiting", studio.preview, waited,
            cache_words(check_state), check_state != kNotCached && check_unwanted ? " (unwanted by stock)" : "");
        ++studio.generation;
        return;
    }
    studio.next_check_ms = t + kCheckMs;
}
void maybe_check(u32 t) {
    if (!studio.loading_preview || studio.checking || studio.compare) return;
    if (static_cast<i32>(t - studio.next_check_ms) < 0) return;
    studio.checking = true;
    check_key = host::sample_key(studio.preview);
    check_serial = studio.shown_serial;
    queue(check_job, check_done);
}
}  // namespace

// ------------------------------------------------------------- the hooks
// The library's keys (the SDK's entry branches, sample_studio_hooks.s): true keeps a key from stock.
bool press_plain(u32 key) {
    if (studio.mode == kClosed) {
        return key == kKeyM1 && !host::shift_held() && open_from_library(false);
    }
    studio.gesture = -1;
    if (bounce.on && key == kKeySample) {
        end_bounce("print stopped", "nothing saved");   // A29: Sample stops a print
        return true;
    }
    if (studio.mode == kSliceChoice) {
        // A27's card: M1 back, M2 new kit (A30), M3 files, M4 this kit (a drum sampler)
        if (key == kKeyM1 || key == kKeySample) {
            studio.mode = kOpen;
            clear_card();
        } else if (key == kKeyM2 || key == kKeyM3 || (key == kKeyM4 && studio.engine == kDrumSampler)) {
            slices_to_kit = key == kKeyM4;
            slices_new_kit = key == kKeyM2;
            slice_choice_made = true;
            studio.mode = kOpen;
            start_save();
        }
        return owned(key);
    }
    if (studio.mode == kLeaving) {
        if (key == kKeyM1) {
            discard_and_close();
        } else if (key == kKeyM2 || key == kKeySample) {
            studio.mode = kOpen;
            clear_card();
        } else if (key == kKeyM4) {
            studio.leave_after_save = true;
            start_save();
        }
    } else if (studio.mode == kOpen) {
        if (key >= kKeyClick0 && key <= kKeyClick3) {
            studio.row ^= 1u;
        } else if (key >= kKeyM1 && key <= kKeyM4) {
            studio.page = static_cast<Page>(key - kKeyM1);
            axis_frames = 0;   // an output page fits its span afresh
            studio.row = 0;
            studio.zoom_marker = 0;
            if (studio.page == kChop) chop_cuts();
            touch_live();
            ++studio.generation;
        } else if (key == kKeySample) {
            leave();
        }
    }
    return owned(key);
}
bool press_shift(u32 key) {
    if (studio.mode == kClosed) {
        return key == kKeyM1 && open_from_library(true);
    }
    studio.gesture = -1;
    if (studio.mode == kOpen) {
        if (key == kKeyM1) {
            undo_step();
        } else if (key == kKeyM2) {
            // A26: Compare plays the original and Play the current edit; each
            // latches and turns to Stop: a press plays, a second stops, the other switches over
            if (play.on && studio.compare) {
                stop_play();
            } else {
                play_original();
                studio.compare = play.on;
            }
        } else if (key == kKeyM3) {
            if (play.on && !studio.compare) stop_play();
            else play_edit();
        } else if (key == kKeyM4) {
            start_save();
        }
    } else if (studio.mode == kLeaving && key == kKeyM4) {
        studio.leave_after_save = true;
        start_save();
    }
    return owned(key);
}
bool release_key(u32 key) { return studio.mode != kClosed && owned(key); }
bool turn_encoder(u32 encoder, i32 detents) {
    if (studio.mode == kClosed) return false;
    if (studio.mode == kOpen && encoder < 4) turn(encoder, detents);
    return true;   // open: every position is the studio's, so stock's list never moves under it
}
static bool hooked_said;
// Every library frame: true when the studio drew it.
bool frame(i32 elapsed) {
    const u32 t = now_ms();
    if (!hooked_said) {
        // The first browser frame since boot: proof the five entry branches are in.
        hooked_said = true;
        log(__LINE__, "hooked: the library's keys and the browser frame; label on");
    }
    if (!library_on_top()) {
        if (studio.mode == kOpen || studio.mode == kLeaving || studio.mode == kSliceChoice || bounce.on) {
            log(__LINE__, "the library left the screen (another browser): closing");
            close_studio(true);
        }
        studio.library_visible = false;
        return false;
    }
    if (!studio.library_visible || t - studio.library_seen_ms > kGapMs) {
        if (studio.mode == kOpen || studio.mode == kLeaving || studio.mode == kSliceChoice || bounce.on) {
            log(__LINE__, "the library back after %d ms away: closing", static_cast<int>(t - studio.library_seen_ms));
            close_studio(true);
        }
        take_snapshot();
    }
    studio.library_visible = true;
    studio.library_seen_ms = t;
    if (studio.mode == kClosed) {
        if (!label_wanted() || !ready_object()) return false;
        host::stock_frame();
        label();
        host::surface_present(output);
        return true;
    }
    if (studio.zoom_marker && static_cast<i32>(t - studio.zoom_until) > 0) {
        studio.zoom_marker = 0;
        ++studio.generation;
    }
    if (studio.mode == kOpen && host::selected_track() != studio.track && !bounce.on) {
        // another track selected under the studio: it closes (the edit kept for later), so the
        // old track sounds again and the new one isn't held
        log(__LINE__, "track changed under the studio (%d, now %d): closed", static_cast<int>(studio.track),
            static_cast<int>(host::selected_track()));
        close_studio(true);
        return false;
    }
    if (!studio.ready && !studio.loading && static_cast<i32>(t - studio.retry_ms) > 0) ask_pcm();
    flush_live(false);
    if (grow_due) {
        grow_due = false;
        if (busy && render.target == kToPreview && render.segmented) send_grow(render.serial, render.synced);
    }
    follow_notes(t);
    follow_live();
    follow_bounce(t);
    keep_playing(t);
    if (files_pending) {
        // A15's proof: stock's refresh clears the byte once it has gathered the keys' files
        const i32 waited = static_cast<i32>(t - files_set_ms);
        if (host::samples_gathered()) {
            files_pending = false;
            log(__LINE__, "stock gathered the keys' files %d ms after the change", waited);
        } else if (waited > 3000) {
            files_pending = false;
            log(__LINE__, "stock hasn't gathered the keys' files 3 s after the change");
        }
    }
    if (studio.save_pending && !busy && studio.mode == kSaving) {
        studio.mode = kOpen;
        start_save();   // the preview has stopped: the save renders the whole edit
    }
    maybe_check(t);
    maybe_render(t);
    if (!ready_object()) {
        blank();
        return true;
    }
    page();
    host::surface_present(output);
    return true;
}
bool press(u32 key, bool shift) { return shift ? press_shift(key) : press_plain(key); }
}}}}  // namespace opxy::extensions::sample_studio::edit
