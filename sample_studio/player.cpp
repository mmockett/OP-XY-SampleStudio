// Sample Studio's own player. It is completely separate from the track's sampler, so it
// inherits none of its filter, speed or fades, and the key stays strictly untouched. The audio
// board loads each render itself (below), rather than through the key and stock's
// loader.
//
// It defines the master bus's two hooks (profile service master_bus: the master EQ call
// 8d511b80 and the B3 receiver 8d5b145e), which a master processor extension defines
// otherwise; tooling/native.py refuses a build with both. It runs on the audio board only
// (the two boards run this same image on two chips, so every global here is the audio
// board's own).
//
// The audio board owns the card (its FatFs is local; the main board's file calls reach it over
// the bridge), so it reads the studio's rendered previews itself: a job on stock's background
// worker task loads the newest preview into one buffer from the sample pool, 32 KB a job,
// outside the audio block. The original (unrendered edits, compare) is already in the audio
// board's sample cache because it is on the key: the block borrows it there by its path's hash.
// The block plays either at unity and the file's rate, after the dry signal and both send
// returns, before the master compressor and volume. Nothing touches the track or the key.
// Each firmware fact the player rests on is under "Assumptions" below.
#include <opxy/sample_studio.hpp>
#include <opxy/audio.hpp>
#include <opxy/q31.hpp>
#include "bake.hpp"
#include "studio_dsp.hpp"

namespace opxy { namespace extensions { namespace sample_studio { namespace play {
using opxy::i32;
using opxy::u32;
using opxy::i16;
using namespace opxy::sample_bake;
namespace dsp = opxy::studio_dsp;
namespace host = opxy::studio;
using host::barrier;
using host::cycles;
using host::now_us;
using host::File;

namespace {
// ---------------------------------------------------------------- Assumptions
// P1 The B3 receiver (stock 8d5b144e's call at 8d5b145e, the hook the master processor
//    also uses) runs in the audio board's comm task and sees every B3 parameter; IDs outside
//    138..152 go on to stock's setter 8d50eb8c unchanged. IDs 138..152 never reach it: it has
//    no bounds check (docs/abi-1.2.1.md). The main board sends them through stock 8d4dc84c.
const u32 kBegin = 138, kSource = 139, kWhich = 140, kStart = 141, kEnd = 142, kLevel = 143, kCommit = 144;
const u32 kHold = 145;   // P7: the studio's track plus one (0: none), on its own
const u32 kGrow = 146, kOnDisk = 147;   // P8: a render's serial | track << 24, then its frames on disk
enum : u32 { kPlay = 1, kReverse = 4, kRelease = 8, kGrowing = 16 };   // begin's flags
enum : u32 { kFromCache = 1, kFromFile = 2, kLive = 3, kLiveFile = 4 };   // source's mode (low byte; track above)
// P15 (editor.cpp A47): kBounceFrames' top bit asks for a "full" tail: the capture stops once the
//    print stays quiet after the play ends (capture_block), at most at the size sent.
// P14 (editor.cpp A44): an armed capture also waits for, and starts with, a kLiveFile play (the beat
//    retime's preview through the live chain), which itself starts only once the preview is loaded whole.
// P13 (editor.cpp A38): a growing preview's loader that was marked done before its last segment (a
//    command naming it without kGrowing reads as its landing) carries on when a larger count arrives.
// P12 (editor.cpp A32): kLiveFile runs P9's chain on the loaded preview (which = its serial)
//    instead of the pinned original: the retime render, already in playing order, at its length, with
//    the gain, colour, DC, fades and channels live over it. It starts once the preview is loaded to the
//    play's end, and stops, as a preview play does, when another preview replaces it in the buffer.
// P9 Live edits (editor.cpp A24): kLive plays the original on the key (pinned by its
//    hash, as kFromCache) through the render's own chain, dsp::Pipeline (studio_dsp.hpp:
//    markers, reverse, gain, tape speed, drive, bits, rate, filter, wow, mix, DC), then the
//    writer's fades and channels (editor.cpp put_frame), frame by frame in the block. Its
//    settings come as kParam words (index << 24 | 24-bit value), published whole by kParamCommit;
//    a play takes the newest set. While it plays, a newer set changes the colour stage and the
//    gain, eased per block (gain 1/8 of the way, drive and mix a few steps, the filter one cutoff
//    a block, so no zipper); markers, reverse, speed, fades, DC and channels apply from the next
//    play. With settings held, the chain is the saved render's sample for sample before the 16-bit
//    write (the file's dither is the only difference: at most 1 LSB, tests/test_sample_studio.py).
//    The block's cost is measured in cycles (CYCLES) and logged with the play.
const u32 kParam = 148, kParamCommit = 149;
enum : u32 { kPBegin, kPEnd, kPReverse, kPGainHigh, kPGainLow, kPDc, kPStretch, kPRate, kPBits, kPDrive, kPSweep,
             kPComp, kPSend1, kPSend2, kPSpare, kPFrames, kPFadeIn, kPFadeOut, kPChans, kParams };
// P10 Live sends (editor.cpp A25), through whichever effects sit in the FX 1 and
//    FX 2 slots: the player's block is made at the first of the mixer's per-track hooks each block
//    (drum_send.cpp's 8d510bea, before any track's voices: the engine clears its five mix buffers,
//    FX I's 0x84caa140 and FX II's 0x84caa340 among them, at 8d5107a8..8d51083c before its track
//    loop) and added into the two send buses at the send amounts, through stock's curves for a
//    track's send knob (FX I 0.8 k^2 + 0.2 k, FX II k), before stock runs the send effects
//    (8d50f594 at 8d5116a0, 8d511908). The dry block goes into the master bus (P2). The
//    amounts come as kPSend1/kPSend2, applied at once (any play), eased per block.

const i32 kSendEase = 1 << 25;   // a full sweep of a send in ~64 blocks (~93 ms)
// P11 The real-time bounce (editor.cpp A29): kBounce arms a capture (its serial; 0 lets it go)
//    and kBounceFrames sizes it, in 44.1 kHz frames. The loader's job allocates the buffer from the
//    sample pool (after the same headroom check), and the next live play waits for it. From that
//    play's first block, the master hook stores the studio's dry block plus FX I's and FX II's buses,
//    which by then hold the send effects' returns (8d50f594 runs them in place before the sum at
//    8d511b2a; FX I's C, FX II's I), back at the file's level (x 2^31 / kKeyGain, rounded to 16 bits).
//    When it's full, the loader's job (run by the main board's kBouncePoke) writes .studio/b<N>.tmp,
//    32 KB a step, and renames it .wav; the main board takes it from there.
const u32 kBounce = 150, kBounceFrames = 151, kBouncePoke = 152;
const char kBounceFolder[] = "/fat32/samples/user/.studio/b";
// P2 The master EQ call at 8d511b80 runs once a block (64 stereo frames, Q31, separate left
//    and right arrays) after dry + FX I + FX II are summed and before the master compressor
//    (8d502828) and master volume. The player mixes after stock's EQ.
const u32 kFrames = opxy::block_frames;
// P3 The audio board's sample cache: the same manager word and map as the main board's
//    (0x84048894, 0x08039f24). The original is pinned by raising its node's use count (+0x18)
//    and released with stock's 8d509df8, only in the block at a command boundary (stock's own
//    deferred note starts lock the same mutex in the block).
// P4 Level, calibrated from the device log: a sampler voice on the key reaches the track's sum at about
//    pcm << 7 (a 31,773 peak file: its silenced voices peaked 56..64 /32768 of full scale at the
//    voice hook), then stock scales that sum by the track's gain (0x08cccccc at the default
//    volume, about -23 dB; drum_send.hpp drum_send_track_gain: a Q31 multiply) and its dry level
//    (0x7fffffff), and the engine adds it into this bus. Mixing pcm << 10 here gives 491 /32768 for
//    the same file, about 41 dB over the key: far too loud. The player
//    mixes at the key's default level, fixed (nothing inherited from the track): pcm << 7
//    times 0x08cccccc, so moving the track's fader doesn't move it. The log still compares
//    its peak with the key's (the silenced voices' peak times the track's gain words).
const i32 kKeyGain = 0x08cccccc >> 9;    // Q31 on gain_word's pcm << 16: pcm << 7 x 0.069
// P5 Timing: the main board resends the playing command every ~500 ms; without one for
//    kTimeoutBlocks the player stops (a lost stop or a closed studio never leaves it playing).
const u32 kTimeoutBlocks = 1100;          // ~1.6 s
const u32 kRampStep = 4;                  // 0..256 over 64 frames: one block in
const u32 kStopStep = 1;                  // 256 frames out (~6 ms): a note-off or a stop fades
const u32 kPrebufferFrames = 4096;        // a preview starts once this much past its start is loaded
// P6 Loading (stock's own sample-load context): jobs posted to the background worker task
//    (bg_worker, priority 1) with stock's 8d4dd728(arg, 0, fn, done), one pending at a time
//    (the queue drops everything at 127); each opens or reads 32 KB of the preview with newlib
//    fopen/fread/fclose (0x8fed0348/05bc/00ae, local on this board) and re-posts itself. The
//    buffer is one block from the sample pool (record 0x08039edc: mutex, then dlmalloc mspace;
//    malloc 8c048788, free 8c048a26, lock 8c03e6f4 / unlock 8c03e9c8; in use 8d509608(),
//    capacity 63 MiB), allocated at the first load after a headroom check, its PCM placed at
//    44 mod 32 so sector-aligned file reads go straight into it, and freed on the studio's
//    release. Never in the block: no file call, allocation or pool lock there.
const u32 kPoolCapacity = host::pool_capacity;
const u32 kMaxFrames = 882000;            // stock's loader limit, and the studio's previews'
const u32 kBufferBytes = kMaxFrames * 2 * 2 + 64;
const u32 kHeadroom = 8u << 20;           // what stock keeps free for its own loads after ours
const u32 kChunkBytes = 32768, kHeader = 44;
// P8 Play while rendering: a full preview render writes its output as segment files of
//    kSegmentFrames, each closed when full: t<track>-<serial>.a.wav, .b.wav, ... (editor.cpp
//    A23; a file reopened to append doesn't hold its place: it reopens at 0). After each,
//    the main board sends kGrow/kOnDisk: the frames on disk in all. The loader reads segment k
//    whole into the buffer at frame k * kSegmentFrames, up to that count; a segmented preview is
//    complete once a command names it without kGrowing (it landed, its last count sent). The quick
//    first part stays one plain file. A play marked kGrowing stops cleanly where what's loaded ends.
const u32 kSegmentFrames = 65536;
const char kStudioFolder[] = "/fat32/samples/user/.studio/t";
// P7 The studio's track is silent while the studio is open (otherwise the keys play the
//    filtered original through the track; they play the edit through this player instead, which
//    the main board starts on each of the track's notes). The mixer's voice loop hands each
//    voice's finished block (0x11b01548, 64 interleaved L/R Q31 frames) to drum_send.cpp's hook
//    at 8d51c7b2 just before stock adds it into the track's sum; the hook calls
//    sdk_studio_voice, which zeroes the voices of the held track (one block's ramp in and out),
//    so stock's notes, envelopes and note-offs run untouched and nothing can stick. The main
//    board resends kHold every ~250 ms while the studio is open and 0 on leave; without one
//    for kTimeoutBlocks the track sounds again.
u32 report_serial;         // the command last logged
volatile u32 hold_value;   // the receiver's: track + 1, or 0
volatile u32 hold_age;     // blocks since it arrived (the block counts)
volatile u32 blocks;       // the block's count, which marks the voice loop's block (bg_worker reads it too)
u32 voice_block = ~0u;     // the block the voice hook last saw
u32 mute_was, mute_now;    // track + 1 silenced in the last block the voices ran, and in this one

typedef void File;
struct Command {
    u32 serial, flags, mode, track, which, start, end;
    i32 level;
};
// ---- P9: the live chain's settings, as the writer takes them
struct Live {
    dsp::Settings s;
    u32 frames, fade_in, fade_out, chans;   // output frames, fade lengths in frames, sample_bake Channels
};
Live staged_live;
u32 staged_gain_high;
volatile u32 live_version;    // odd while the receiver writes
Live published_live;
// ---- the B3 receiver (comm task): staged, then published whole under a sequence count
Command staged;
u32 staged_mask;
bool staging;
volatile u32 published_version;   // odd while the receiver writes
Command published;

// ---- the loader (bg_worker), asked by the receiver
volatile u32 want_serial, want_track;   // the preview to hold (0: none)
volatile bool want_release;
volatile bool job_pending;
unsigned char* block;                   // the sample-pool block
i16* pcm;                               // its PCM, at 44 mod 32
bool refused;                           // the headroom check failed this boot
volatile u32 bounce_want, bounce_frames;   // P11: the receiver's arm (serial, size), 0: none
volatile bool bounce_cancel;
u32 bounce_serial;                      // the capture's serial
volatile u32 bounce_done;               // the last serial finished, failed or let go: idle again
unsigned char* bounce_block;            // its sample-pool block
i16* bounce_pcm;
u32 bounce_total;
volatile bool bounce_ready, bounce_capturing, bounce_full;
volatile bool bounce_quiet_want;        // P15: the next arm stops at quiet after the play
bool bounce_until_quiet, bounce_play_ended;
u32 bounce_loud_at;                     // P15: the last frame above kQuietLevel after the play ended
volatile u32 bounce_at;                 // frames captured
File* bounce_file;
u32 bounce_written;                     // frames written
bool bounce_failed;
volatile u32 want_limit;                // P8: frames of it on disk in all (segmented)
volatile bool want_complete;            // P8: no more segments to come
volatile bool want_resume;              // P13: a larger count came for the preview loading or loaded
volatile bool want_segmented;           // P8: segment files (kGrow named it), else one plain file
u32 grow_serial, grow_track;            // P8: the receiver's kGrow, waiting for its kOnDisk
bool grow_staged;
bool segmented;                         // the buffer's preview is segment files
u32 file_start, file_end;               // frames of the buffer the open file fills
bool load_done;                         // the preview is loaded to its end
u32 opens;
volatile u32 load_serial;               // the preview the buffer holds or is loading
volatile u32 loaded_frames;             // frames of it in the buffer (published after each chunk)
volatile u32 load_channels, load_rate, load_total;
File* file;
u32 load_started, chunks, worst_us, read_us;

// ---- the block
struct Player {
    u32 version, serial;
    Command playing;    // what sounds (or fades)
    Command next;       // what waits to start: never mixed with a fading `playing`
    bool waiting, active, pinned, from_file;
    u32 key;
    const i16* data;
    u32 channels, rate, file_serial;
    u32 pos, frac, step, gain, ramp;
    bool stopping;
    u32 quiet;
    bool live;          // P9: the chain plays
    u32 wait_blocks;    // blocks a play has waited to start
} player;
// P9: the chain in the block
dsp::Pipeline chain;
Live chain_now, chain_target;
u32 chain_version, chain_out, chain_step_in, chain_step_out;
u32 chain_worst, chain_sum, chain_blocks;
i32 chain_l[kFrames], chain_r[kFrames];   // the chain's block (P9)
u32 chain_at, chain_count;
i32 rs_window[2][2];                      // a source not at 44.1 kHz, read at its rate
u32 rs_frac, rs_step;
bool rs_primed;
// ---- what the block saw, for the receiver's log lines (races only blur a log)
struct Report {
    u32 serial, mode, which, start_blocks, frames, rate, channels, peak, voice_peak, worst_cycles, mean_cycles;
    u32 setting[8];     // P9: the live chain's stretch, rate, bits, drive, sweep, comp, dc, source rate
    bool started, starved;
} report;
volatile i32 track_gain, track_level;   // stock's gain words for the held track (P4)

void log_line(u32 line, const char* format, int a, int b, int c, int d, int e, int f) {
    host::log_audio(line, format, a, b, c, d, e, f);
}
// ---- the sample pool (P6)
void* pool_alloc(u32 bytes) { return host::pool_allocate(bytes); }
void pool_free(void* p) { host::pool_free(p); }
u32 pool_in_use() { return host::pool_in_use(); }
// ---- files (local on this board, P6)
File* open_file(const char* path) { return host::file_open(path, "rb"); }
u32 read_file(void* to, u32 bytes, File* f) { return host::file_read(to, 1, bytes, f); }
void close_file(File* f) { host::file_close(f); }
File* create_file(const char* path) { return host::file_open(path, "wb"); }
u32 write_file(const void* from, u32 bytes, File* f) { return host::file_write(from, 1, bytes, f); }
bool rename_file(const char* from, const char* to) { return host::rename_file(from, to); }
void put32(unsigned char* p, u32 v) { p[0] = v & 255u; p[1] = (v >> 8) & 255u; p[2] = (v >> 16) & 255u; p[3] = v >> 24; }
u32 le16(const unsigned char* p) { return p[0] | (p[1] << 8); }
u32 le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }
void preview_path(char* out, u32 size, u32 track, u32 serial, u32 segment) {
    u32 n = 0;
    out[0] = 0;
    append(out, size, n, kStudioFolder);
    append_number(out, size, n, track + 1);
    append(out, size, n, "-");
    append_number(out, size, n, serial);
    if (segment != ~0u) {
        const char letter[3] = {'.', static_cast<char>('a' + segment), 0};
        append(out, size, n, letter);
    }
    append(out, size, n, ".wav");
}

void load_step(u32);
u32 job_posted_block;
void post_load() {
    // review: a job stock's queue dropped (it drops everything at 127) would leave job_pending set
    // for good; after ~2 s without the job running, post again
    if (job_pending && blocks - job_posted_block < 1400u) return;
    job_pending = true;
    job_posted_block = blocks;
    host::post(load_step, nullptr);
}
void bounce_path(char* out, u32 size, u32 serial, const char* tail) {
    u32 n = 0;
    out[0] = 0;
    append(out, size, n, kBounceFolder);
    append_number(out, size, n, serial);
    append(out, size, n, tail);
}
// A sample-pool block the audio block may still be reading (a play fading out, a capture's
// last block) is freed only once the block count has moved on by kRetireBlocks; until then the
// loader's job re-posts itself.
const u32 kRetireBlocks = 8;
unsigned char* retired[3];
u32 retired_at[3];
void retire(unsigned char* b) {
    if (!b) return;
    for (u32 i = 0; i < 3; ++i)
        if (!retired[i]) {
            retired[i] = b;
            retired_at[i] = blocks;
            return;
        }
    pool_free(b);   // no slot left (never expected): free at once
}
bool reap() {
    bool left = false;
    for (u32 i = 0; i < 3; ++i) {
        if (!retired[i]) continue;
        if (blocks - retired_at[i] >= kRetireBlocks) {
            pool_free(retired[i]);
            retired[i] = nullptr;
        } else {
            left = true;
        }
    }
    return left;
}
// review: ready first (start() reads it), then the rest; the serial is retired, not forgotten, so
// a finished bounce never re-arms nor holds every later live play
void bounce_free() {
    if (bounce_file) close_file(bounce_file);
    bounce_file = nullptr;
    bounce_ready = false;
    barrier();
    bounce_capturing = false;
    bounce_full = false;
    barrier();
    retire(bounce_block);
    bounce_block = nullptr;
    bounce_pcm = nullptr;
    if (bounce_serial) bounce_done = bounce_serial;
    bounce_serial = 0;
}
// P11: the main board learns of a failure from b<N>.fail, and no .tmp is left behind.
void bounce_fail(u32 serial) {
    char path[64];
    bounce_path(path, sizeof path, serial, ".tmp");
    host::remove_file(path);
    bounce_path(path, sizeof path, serial, ".fail");
    File* f = create_file(path);
    if (f) close_file(f);
}
// P11, on bg_worker: arm, then write. True while there's more to do.
bool bounce_step() {
    if (bounce_cancel) {
        bounce_cancel = false;
        if (bounce_serial) {
            log_line(__LINE__, "bounce %d: let go", static_cast<int>(bounce_serial), 0, 0, 0, 0, 0);
            const u32 serial = bounce_serial;
            bounce_free();
            bounce_fail(serial);
        }
        return false;
    }
    if (bounce_want && bounce_want != bounce_serial && bounce_want != bounce_done) {
        bounce_free();
        bounce_serial = bounce_want;
        bounce_total = bounce_frames;
        bounce_at = bounce_written = 0;
        bounce_until_quiet = bounce_quiet_want;   // P15
        bounce_play_ended = false;
        bounce_loud_at = 0;
        bounce_failed = false;
        const u32 bytes = bounce_total * 4u + 64u;
        const u32 used = pool_in_use();
        if (!bounce_total || used + bytes + kHeadroom > kPoolCapacity) {
            log_line(__LINE__, "bounce %d: no room for %d bytes (sample pool in use %d)", static_cast<int>(bounce_serial),
                     static_cast<int>(bytes), static_cast<int>(used), 0, 0, 0);
            const u32 serial = bounce_serial;
            bounce_free();
            bounce_fail(serial);
            return false;
        }
        bounce_block = static_cast<unsigned char*>(pool_alloc(bytes));
        if (!bounce_block) {
            const u32 serial = bounce_serial;
            bounce_free();
            bounce_fail(serial);
            return false;
        }
        bounce_pcm = reinterpret_cast<i16*>(bounce_block + ((kHeader - reinterpret_cast<u32>(bounce_block)) & 31u));
        barrier();
        bounce_ready = true;
        log_line(__LINE__, "bounce %d: %d frames ready to capture (sample pool in use %d -> %d)", static_cast<int>(bounce_serial),
                 static_cast<int>(bounce_total), static_cast<int>(used), static_cast<int>(pool_in_use()), 0, 0);
        return false;
    }
    if (!bounce_full || !bounce_pcm) return false;
    char path[64];
    if (!bounce_file && bounce_written == 0) {
        bounce_path(path, sizeof path, bounce_serial, ".tmp");
        bounce_file = create_file(path);
        unsigned char header[kHeader];
        const u32 data = bounce_total * 4u;
        __builtin_memcpy(header, "RIFF", 4); put32(header + 4, 36u + data);
        __builtin_memcpy(header + 8, "WAVEfmt ", 8); put32(header + 16, 16);
        header[20] = 1; header[21] = 0; header[22] = 2; header[23] = 0;
        put32(header + 24, 44100); put32(header + 28, 44100u * 4u);
        header[32] = 4; header[33] = 0; header[34] = 16; header[35] = 0;
        __builtin_memcpy(header + 36, "data", 4); put32(header + 40, data);
        if (!bounce_file || write_file(header, kHeader, bounce_file) != kHeader) {
            log_line(__LINE__, "bounce %d: couldn't create its file", static_cast<int>(bounce_serial), 0, 0, 0, 0, 0);
            const u32 serial = bounce_serial;
            bounce_free();
            bounce_fail(serial);
            return false;
        }
    }
    if (!bounce_file) return false;
    if (bounce_written < bounce_total) {
        const u32 room = kChunkBytes / 4u;
        const u32 n = bounce_total - bounce_written < room ? bounce_total - bounce_written : room;
        if (write_file(bounce_pcm + bounce_written * 2u, n * 4u, bounce_file) != n * 4u) {
            log_line(__LINE__, "bounce %d: the card refused a write", static_cast<int>(bounce_serial), 0, 0, 0, 0, 0);
            const u32 serial = bounce_serial;
            bounce_free();
            bounce_fail(serial);
            return false;
        }
        bounce_written += n;
        return true;
    }
    close_file(bounce_file);
    bounce_file = nullptr;
    char done[64];
    bounce_path(path, sizeof path, bounce_serial, ".tmp");
    bounce_path(done, sizeof done, bounce_serial, ".wav");
    const bool ok = rename_file(path, done);
    log_line(__LINE__, "bounce %d: %d frames written, renamed %d", static_cast<int>(bounce_serial),
             static_cast<int>(bounce_total), ok ? 1 : 0, 0, 0, 0);
    const u32 serial = bounce_serial;
    bounce_free();
    if (!ok) bounce_fail(serial);
    return false;
}
void log_loaded() {
    log_line(__LINE__, "loader: preview %d loaded, %d frames x %d ch in %d chunks, %d files, %d ms", static_cast<int>(load_serial),
             static_cast<int>(loaded_frames), static_cast<int>(load_channels), static_cast<int>(chunks),
             static_cast<int>(opens), static_cast<int>((now_us() - load_started) / 1000u));
    log_line(__LINE__, "loader: reads %d ms, worst chunk %d us", static_cast<int>(read_us / 1000u),
             static_cast<int>(worst_us), 0, 0, 0, 0);
}
// bg_worker: one step of the loader.
void load_work();
void load_step(u32) {
    job_pending = false;
    const bool waiting = reap();
    load_work();
    if (waiting || reap()) post_load();   // a retired block still to free
}
void load_work() {
    if (bounce_step()) {
        post_load();   // P11: more of the bounce to write
        return;
    }
    if (want_release) {
        want_release = false;
        if (file) close_file(file);
        file = nullptr;
        load_serial = 0;
        loaded_frames = 0;
        pcm = nullptr;
        barrier();
        retire(block);   // the player may be fading out of it
        block = nullptr;
        log_line(__LINE__, "loader: buffer let go, sample pool in use %d", static_cast<int>(pool_in_use()), 0, 0, 0, 0, 0);
        return;
    }
    if (want_serial && want_serial != load_serial) {
        // a new preview: the buffer starts again
        if (file) close_file(file);
        file = nullptr;
        load_serial = want_serial;
        loaded_frames = 0;
        load_total = 0;
        load_done = false;
        want_resume = false;
        segmented = want_segmented;
        barrier();
        if (!block && !refused) {
            const u32 used = pool_in_use();
            if (used + kBufferBytes + kHeadroom > kPoolCapacity) {
                refused = true;
                log_line(__LINE__, "loader: sample pool in use %d of %d: no room for the preview buffer (%d)",
                         static_cast<int>(used), static_cast<int>(kPoolCapacity), static_cast<int>(kBufferBytes), 0, 0, 0);
            } else {
                block = static_cast<unsigned char*>(pool_alloc(kBufferBytes));
                const u32 base = reinterpret_cast<u32>(block);
                pcm = block ? reinterpret_cast<i16*>(block + ((kHeader - base) & 31u)) : nullptr;   // 44 mod 32 (P6)
                log_line(__LINE__, "loader: preview buffer %d bytes at %x, sample pool in use %d -> %d",
                         static_cast<int>(kBufferBytes), static_cast<int>(base), static_cast<int>(used),
                         static_cast<int>(pool_in_use()), 0, 0);
                if (!block) refused = true;
            }
        }
        load_started = now_us();
        chunks = worst_us = read_us = opens = 0;
    }
    if (want_resume) {
        // P13: a segmented preview marked done too soon (a stop read as its landing) whose
        // count has grown since: on from its next segment, which starts where it stopped (a segment boundary)
        want_resume = false;
        if (load_done && segmented && !file && loaded_frames < want_limit && loaded_frames < kMaxFrames &&
            !(loaded_frames % kSegmentFrames)) {
            load_done = false;
            log_line(__LINE__, "loader: preview %d grew past %d frames: loading on", static_cast<int>(load_serial),
                     static_cast<int>(loaded_frames), 0, 0, 0, 0);
        }
    }
    if (!pcm || !load_serial || load_done) return;
    // a segmented preview fills to the frames on disk; a plain one to its header's length
    u32 limit = segmented ? (want_limit < kMaxFrames ? want_limit : kMaxFrames) : ~0u;
    if (segmented) load_total = limit;
    if (!file && segmented && loaded_frames >= limit) {
        if (want_complete) {
            load_done = true;
            log_loaded();
        }
        return;   // the next segment isn't there yet
    }
    if (!file) {
        // the next file: segment k starts at frame k * kSegmentFrames, so a segment opens only on that boundary
        const u32 segment = segmented ? loaded_frames / kSegmentFrames : ~0u;
        char path[64];
        preview_path(path, sizeof path, want_track, load_serial, segment);
        file = open_file(path);
        ++opens;
        unsigned char header[kHeader];
        if (!file || read_file(header, kHeader, file) != kHeader || le32(header) != 0x46464952u ||
            le32(header + 8) != 0x45564157u || le32(header + 36) != 0x61746164u || le16(header + 34) != 16 ||
            (loaded_frames && le16(header + 22) != load_channels) || (segmented && loaded_frames % kSegmentFrames)) {
            log_line(__LINE__, "loader: couldn't open or read t%d-%d, file %d", static_cast<int>(want_track + 1),
                     static_cast<int>(load_serial), static_cast<int>(segment == ~0u ? -1 : static_cast<int>(segment)), 0, 0, 0);
            if (file) close_file(file);
            file = nullptr;
            load_done = true;   // until a newer preview is named
            return;
        }
        if (!loaded_frames) {
            const u32 channels = le16(header + 22);
            load_channels = channels < 1 ? 1 : channels > 2 ? 2 : channels;
            load_rate = le32(header + 24);
        }
        const u32 frames = le32(header + 40) / (load_channels * 2);
        file_start = loaded_frames;
        if (segmented) {
            file_end = file_start + kSegmentFrames < limit ? file_start + kSegmentFrames : limit;   // the count is the truth
        } else {
            file_end = frames > kMaxFrames ? kMaxFrames : frames;
            load_total = file_end;
        }
    }
    if (file && loaded_frames < file_end) {
        // one read, up to the next 32 KB boundary of the file: sector-aligned reads (P6)
        const u32 frame_bytes = load_channels * 2;
        const u32 offset = kHeader + (loaded_frames - file_start) * frame_bytes;
        u32 room = (kChunkBytes - offset % kChunkBytes) / frame_bytes;
        if (!room) room = kChunkBytes / frame_bytes;
        const u32 n = file_end - loaded_frames < room ? file_end - loaded_frames : room;
        const u32 t0 = now_us();
        const u32 got = read_file(pcm + loaded_frames * load_channels, n * frame_bytes, file) / frame_bytes;
        const u32 us = now_us() - t0;
        read_us += us;
        if (us > worst_us) worst_us = us;
        ++chunks;
        barrier();
        loaded_frames = loaded_frames + got;
        if (got < n) {
            // the file ended early: a plain file's end, or a segment cut short (then stop here)
            if (!segmented) load_total = loaded_frames;
            else log_line(__LINE__, "loader: segment %d of preview %d ended at frame %d", static_cast<int>(file_start / kSegmentFrames),
                          static_cast<int>(load_serial), static_cast<int>(loaded_frames), 0, 0, 0);
            file_end = loaded_frames;
            load_done = true;
            close_file(file);
            file = nullptr;
            if (!segmented) log_loaded();
            return;
        }
    }
    if (file && loaded_frames >= file_end) {
        close_file(file);
        file = nullptr;
        if (!segmented) {
            load_done = true;
            log_loaded();
        } else if (loaded_frames >= limit && want_complete) {
            load_done = true;
            log_loaded();
        }
    }
    const bool more = !load_done && (file || (segmented ? loaded_frames < want_limit : true));
    if (want_release || (want_serial && want_serial != load_serial) || more) post_load();
}

// ---- the original, borrowed from the cache (P3)
const void* pin(u32 key) { return host::pin(key); }
void unpin() {
    if (!player.pinned) return;
    host::unpin(player.key);
    player.pinned = false;
}
bool take(Command& out) {
    const u32 v = published_version;
    if (v & 1u || v == player.version) return false;
    barrier();
    out = published;
    barrier();
    if (published_version != v) return false;
    player.version = v;
    return true;
}
// Start what the command names, if it is ready: the original pinned, or enough of the preview loaded.
bool start_live(const Command& c);
bool start(const Command& c) {
    player.from_file = c.mode == kFromFile || c.mode == kLiveFile;
    player.live = false;
    if (c.mode == kLiveFile) {
        // P14 (editor.cpp A44): a beat retime's print plays its preview live; the capture as P11
        if (bounce_want && bounce_want != bounce_done && !(bounce_want == bounce_serial && bounce_ready)) return false;
        if (!start_live(c)) return false;   // P12
        if (bounce_ready && !bounce_capturing && !bounce_full) bounce_capturing = true;   // P14: from this block
    } else if (c.mode == kLive) {
        // P11: an armed capture's buffer comes first (a failed or finished one is done: play without it)
        if (bounce_want && bounce_want != bounce_done && !(bounce_want == bounce_serial && bounce_ready)) return false;
        if (!start_live(c)) return false;
        if (bounce_ready && !bounce_capturing && !bounce_full) bounce_capturing = true;   // P11: from this block
    } else if (player.from_file) {
        if (load_serial != c.which || !pcm) return false;
        const u32 have = loaded_frames, end = c.end < load_total ? c.end : load_total;
        if (!have || (have < end && have < c.start + kPrebufferFrames)) return false;
        player.data = pcm;
        player.channels = load_channels;
        player.rate = load_rate;
        player.file_serial = c.which;
    } else {
        const void* object = pin(c.which);
        if (!object) return false;
        const opxy::SampleView view = host::sample_view(object);
        if (!view.data || c.end > view.frames) {
            host::unpin(c.which);
            return false;
        }
        player.pinned = true;
        player.key = c.which;
        player.data = view.data;
        player.channels = view.channels;
        player.rate = view.rate;
    }
    player.playing = c;
    player.pos = c.flags & kReverse ? c.end - 1 : c.start;
    player.frac = 0;
    player.step = ((player.rate << 10) / 44100u) << 6;   // input frames per output frame, Q16
    player.gain = level_gain(c.level);
    player.ramp = 0;
    player.stopping = false;
    player.active = true;
    report.started = true;
    report.start_blocks = blocks - report.start_blocks;   // blocks from the command to the start
    report.rate = player.rate;
    report.channels = player.channels;
    return true;
}
// P9: the newest settings, if a whole set has been published.
bool take_live(Live& out) {
    const u32 v = live_version;
    if (!v || (v & 1u)) return false;
    barrier();
    out = published_live;
    barrier();
    if (live_version != v) return false;
    chain_version = v;
    return true;
}
// P9: start the chain on the pinned original.
bool start_live(const Command& c) {
    Live l;
    if (!take_live(l)) return false;
    opxy::SampleView view = {};
    if (c.mode == kLiveFile) {
        // P12: the loaded preview, to the play's end
        if (load_serial != c.which || !pcm || loaded_frames < l.s.end) return false;
        view.data = pcm;
        view.frames = loaded_frames;
        view.channels = load_channels;
        view.rate = load_rate;
        if (l.s.begin >= l.s.end || l.s.beat || !l.frames || view.channels > 2 || !view.channels) return false;
        player.file_serial = c.which;
    } else {
        const void* object = pin(c.which);
        if (!object) return false;
        view = host::sample_view(object);
        if (!view.data || l.s.end > view.frames || l.s.begin >= l.s.end || l.s.beat || !l.frames || view.channels > 2 ||
            !view.channels) {
            host::unpin(c.which);
            return false;
        }
        player.pinned = true;
    }
    player.key = c.which;
    player.channels = view.channels;
    player.rate = view.rate;
    chain.start(view.data, view.channels, view.rate, l.s);
    chain_now = chain_target = l;
    chain_out = 0;
    chain_at = chain_count = 0;
    rs_frac = 0;
    rs_primed = false;
    rs_step = view.rate == 44100u || !view.rate ? 65536u : (view.rate << 10) / 44100u << 6;   // source frames per output frame
    report.setting[0] = l.s.stretch;
    report.setting[1] = l.s.rate_index;
    report.setting[2] = l.s.bits_index;
    report.setting[3] = l.s.drive;
    report.setting[4] = static_cast<u32>(l.s.sweep);
    report.setting[5] = l.s.comp;
    report.setting[6] = l.s.dc;
    report.setting[7] = view.rate;
    chain_step_in = l.fade_in ? kUnity / l.fade_in : 0;
    chain_step_out = l.fade_out ? kUnity / l.fade_out : 0;
    chain_worst = chain_sum = chain_blocks = 0;
    player.live = true;
    return true;
}
// P10: a send's gain from its amount (editor.cpp send_gain, stock's knob curves).
i32 send_gain(u32 send, u32 amount) {
    const i32 k = static_cast<i32>((0x7fffffffu / 100u) * (amount > 100 ? 100u : amount));
    if (send) return k;
    const i32 shape = mul31(k, static_cast<i32>(0x66666666u)) + static_cast<i32>(0x1999999au);   // 0.8 k + 0.2
    return mul31(k, shape);
}
volatile u32 send_amount[2];
i32 send_now[2];
i32 studio_l[kFrames], studio_r[kFrames];
bool block_made;
// P9: one block's easing of a newer set's colour and gain toward it.
u32 ease(u32 now, u32 target, u32 step) {
    if (now < target) return target - now > step ? now + step : target;
    return now - target > step ? now - step : target;
}
void ease_chain() {
    Live l;
    if (live_version != chain_version && take_live(l)) chain_target = l;
    dsp::Settings& s = chain.s;
    const dsp::Settings& t = chain_target.s;
    if (s.gain != t.gain) s.gain = s.gain < t.gain ? s.gain + ((t.gain - s.gain + 7u) >> 3) : s.gain - ((s.gain - t.gain + 7u) >> 3);
    s.drive = ease(s.drive, t.drive, 4);
    s.comp = ease(s.comp, t.comp, 4);
    if (!s.sweep || !t.sweep || (s.sweep < 0) != (t.sweep < 0)) s.sweep = t.sweep;   // on, off and over at once
    else s.sweep += s.sweep < t.sweep ? 1 : s.sweep > t.sweep ? -1 : 0;           // the cutoff glides
    s.bits_index = t.bits_index;
    s.rate_index = t.rate_index;
}
// P9: the block, live: the chain, the writer's fades and channels, then the key's level (P4).
// P9: the chain's next frame after the writer's fades and channels (editor.cpp put_frame),
// from a block the chain fills 64 frames at a time. False at the end.
bool pull(i32* w) {
    const Live& l = chain_now;
    if (chain_at == chain_count) {
        const u32 left = chain_out < l.frames ? l.frames - chain_out : 0;
        chain_count = left ? chain.fill(chain_l, chain_r, left < kFrames ? left : kFrames) : 0;
        chain_at = 0;
        if (!chain_count) return false;
    }
    w[0] = chain_l[chain_at];
    w[1] = chain_r[chain_at];
    ++chain_at;
    u32 g = kUnity;
    if (chain_out < l.fade_in) g = chain_out * chain_step_in;
    const u32 back = l.frames - 1 - chain_out;
    if (back < l.fade_out && back * chain_step_out < g) g = back * chain_step_out;
    if (g < kUnity)
        for (u32 c = 0; c < player.channels && c < 2; ++c) w[c] = fade_word(w[c], g);
    if (player.channels < 2) w[1] = w[0];
    else if (l.chans == kMono) w[0] = w[1] = mono_word(w[0], w[1]);
    else if (l.chans == kLeft) w[1] = w[0];
    else if (l.chans == kRight) w[0] = w[1];
    ++chain_out;
    return true;
}
// P9: the block, live: the chain, then the key's level (P4). A source not at 44.1 kHz is read at its
// rate (else a 48 kHz file plays 1.5 semitones flat), linearly between frames.
void live_block(i32* left, i32* right) {
    const u32 c0 = cycles();
    ease_chain();
    u32 peak = report.peak;
    for (u32 i = 0; i < kFrames; ++i) {
        if (player.stopping) {
            if (player.ramp <= kStopStep) { player.ramp = 0; break; }
            player.ramp -= kStopStep;
        } else if (player.ramp < 256) {
            player.ramp += kRampStep;
        }
        i32 w[2] = {0, 0};
        if (rs_step == 65536u) {
            if (!pull(w)) player.stopping = true;
        } else {
            // window[0] is frame floor(pos), window[1] the next; step them on as the position passes
            bool ok = true;
            if (!rs_primed) {
                ok = pull(rs_window[0]) && pull(rs_window[1]);
                rs_primed = true;
            }
            while (ok && rs_frac >= 65536u) {
                rs_window[0][0] = rs_window[1][0];
                rs_window[0][1] = rs_window[1][1];
                ok = pull(rs_window[1]);
                rs_frac -= 65536u;
            }
            if (!ok) {
                player.stopping = true;
            } else {
                const i32 f = static_cast<i32>(rs_frac << 15);
                for (u32 c = 0; c < 2; ++c) w[c] = dsp::lerp(rs_window[0][c], rs_window[1][c], f);
                rs_frac += rs_step;
            }
        }
        ++report.frames;
        for (u32 c = 0; c < 2; ++c) {
            i32 x = w[c];
            if (player.ramp < 256) x = (x >> 8) * static_cast<i32>(player.ramp);
            x = opxy::q31::multiply(x, kKeyGain);
            const u32 m = static_cast<u32>(x < 0 ? -x : x);
            if (m > peak) peak = m;
            w[c] = x;
        }
        left[i] = saturate_add(left[i], w[0]);
        right[i] = saturate_add(right[i], w[1]);
    }
    report.peak = peak;
    if (player.stopping && !player.ramp) {
        player.active = false;
        player.live = false;
        unpin();
    }
    const u32 spent = cycles() - c0;
    if (spent > chain_worst) {
        // the settings the worst block ran with (eased knobs included), for the log
        chain_worst = spent;
        report.setting[0] = chain.s.stretch;
        report.setting[1] = chain.s.rate_index;
        report.setting[2] = chain.s.bits_index;
        report.setting[3] = chain.s.drive;
        report.setting[4] = static_cast<u32>(chain.s.sweep);
        report.setting[5] = chain.s.comp;
        report.setting[6] = chain.s.dc;
    }
    chain_sum += spent;
    ++chain_blocks;
    report.worst_cycles = chain_worst;
    report.mean_cycles = chain_blocks ? chain_sum / chain_blocks : 0;
}
// The receiver, at a new command: the play that ends with it, then the command (P4, P8).
u32 logged_blocks, unlogged;
void log_command(const Command& c) {
    // notes from the sequencer can start a play every few blocks: at most one report per ~0.3 s
    if (blocks - logged_blocks < 200u) {
        ++unlogged;
        report = Report{};
        report.start_blocks = blocks;
        if (c.flags & kPlay) { report.serial = c.serial; report.mode = c.mode; report.which = c.which; }
        return;
    }
    logged_blocks = blocks;
    if (unlogged) log_line(__LINE__, "player: %d commands not logged", static_cast<int>(unlogged), 0, 0, 0, 0, 0);
    unlogged = 0;
    if (report.serial) {
        // peaks in 1/2^20 of the bus's full scale; the key's: its silenced voices x the track's gain and dry level
        const i32 key = opxy::q31::multiply(opxy::q31::multiply(static_cast<i32>(report.voice_peak), track_gain), track_level);
        log_line(__LINE__, "player: play %d: started %d, %d frames, peak %d, the key's would be %d (/1048576 of the bus), "
                 "stopped at what was loaded %d", static_cast<int>(report.serial), report.started ? 1 : 0,
                 static_cast<int>(report.frames), static_cast<int>(report.peak >> 11), static_cast<int>(key >> 11),
                 report.starved ? 1 : 0);
        log_line(__LINE__, "player: it started %d blocks after its command, rate %d, %d ch; the key's silenced voices "
                 "peaked %d; track gain %x, dry level %x", static_cast<int>(report.start_blocks), static_cast<int>(report.rate),
                 static_cast<int>(report.channels), static_cast<int>(report.voice_peak >> 16), track_gain, track_level);
    }
    if (report.serial && (report.mode == kLive || report.mode == kLiveFile)) {
        log_line(__LINE__, "player: play %d live: the chain's block took %d cycles at worst, %d on average (of %d a block)",
                 static_cast<int>(report.serial), static_cast<int>(report.worst_cycles), static_cast<int>(report.mean_cycles),
                 580352, 0, 0);
        log_line(__LINE__, "player: play %d live settings: stretch %d rate %d bits %d drive %d sweep %d",
                 static_cast<int>(report.serial), static_cast<int>(report.setting[0]), static_cast<int>(report.setting[1]),
                 static_cast<int>(report.setting[2]), static_cast<int>(report.setting[3]),
                 static_cast<int>(static_cast<i32>(report.setting[4])));
        log_line(__LINE__, "player: play %d live settings: comp %d dc %d source %d Hz, sends %d %d",
                 static_cast<int>(report.serial), static_cast<int>(report.setting[5]), static_cast<int>(report.setting[6]),
                 static_cast<int>(report.setting[7]), static_cast<int>(send_amount[0]), static_cast<int>(send_amount[1]));
    }
    report = Report{};
    report.start_blocks = blocks;
    if (!(c.flags & kPlay)) return;
    report.serial = c.serial;
    report.mode = c.mode;
    report.which = c.which;
    // source: 1 the original (which = its hash), 2 a preview (which = its serial), 3 a preview still rendering,
    // 4 the original through the live chain (P9), 5 the preview through it (P12)
    log_line(__LINE__, "player: play %d asked: source %d which %d, frames %d..%d, level %d", static_cast<int>(c.serial),
             c.mode == kFromCache ? 1 : c.mode == kLive ? 4 : c.mode == kLiveFile ? 5 : c.flags & kGrowing ? 3 : 2,
             static_cast<int>(c.which),
             static_cast<int>(c.start), static_cast<int>(c.end), static_cast<int>(c.level));
}
i32 at(u32 f, u32 c) { return player.data[f * player.channels + (c < player.channels ? c : 0)]; }
}  // namespace

void receive(u32 channel, i32 value) {
    const u32 id = 138 + channel;
    if (id == kHold) {
        hold_value = static_cast<u32>(value) <= 16 ? static_cast<u32>(value) : 0;
        hold_age = 0;
        return;
    }
    if (id == kBounce) {
        // P11: a capture armed (its serial), or let go (0)
        if (!value) {
            bounce_want = 0;
            bounce_cancel = true;
        } else {
            bounce_want = static_cast<u32>(value);   // kBounceFrames came first (editor.cpp)
        }
        post_load();
        return;
    }
    if (id == kBounceFrames) {
        const u32 v = static_cast<u32>(value);
        bounce_quiet_want = (v & 0x80000000u) != 0;   // P15: "full" tail: on until quiet
        const u32 frames = v & 0x7fffffffu;
        bounce_frames = frames <= 4000000u ? frames : 4000000u;
        return;
    }
    if (id == kBouncePoke) {
        if (bounce_full) post_load();   // P11: the writer's next step
        return;
    }
    if (id == kParam) {
        // P9: one setting of the next set
        const u32 v = static_cast<u32>(value), x = v & 0xffffffu;
        Live& l = staged_live;
        switch (v >> 24) {
        case kPBegin: l.s.begin = x; break;
        case kPEnd: l.s.end = x; break;
        case kPReverse: l.s.reverse = x; break;
        case kPGainHigh: staged_gain_high = x; break;
        case kPGainLow: l.s.gain = (staged_gain_high << 16) | (x & 0xffffu); break;
        case kPDc: l.s.dc = x; break;
        case kPStretch: l.s.stretch = x < 25 ? 25 : x > 400 ? 400 : x; break;
        case kPRate: l.s.rate_index = x < 9 ? x : 0; break;
        case kPBits: l.s.bits_index = x < 9 ? x : 0; break;
        case kPDrive: l.s.drive = x <= 100 ? x : 100; break;
        case kPSweep: {
            const i32 v = static_cast<i32>(x & 0x800000u ? x | 0xff000000u : x);   // 24 bits, signed
            const i32 limit = static_cast<i32>(dsp::kCutoffCount);
            l.s.sweep = v < -limit ? -limit : v > limit ? limit : v;
            break;
        }
        case kPComp: l.s.comp = x <= 100 ? x : 100; break;
        case kPSend1: send_amount[0] = x <= 100 ? x : 100; break;   // P10: at once, any play
        case kPSend2: send_amount[1] = x <= 100 ? x : 100; break;
        case kPFrames: l.frames = x; break;
        case kPFadeIn: l.fade_in = x; break;
        case kPFadeOut: l.fade_out = x; break;
        case kPChans: l.chans = x < 4 ? x : 0; break;
        default: break;
        }
        return;
    }
    if (id == kParamCommit) {
        if (!staged_live.s.stretch) staged_live.s.stretch = 100;   // review: never a 0 the resampler divides by
        staged_live.s.beat = 0;
        staged_live.s.pitch = 0;
        staged_live.s.grain_ms = 0;
        live_version = live_version + 1;   // odd: writing
        barrier();
        published_live = staged_live;
        barrier();
        live_version = live_version + 1;
        return;
    }
    if (id == kGrow) {
        grow_serial = static_cast<u32>(value) & 0xffffffu;
        grow_track = (static_cast<u32>(value) >> 24) & 0xffu;
        grow_staged = true;
        return;
    }
    if (id == kOnDisk) {
        // P8: the render on the main board has this much of its preview on disk
        if (grow_staged && grow_serial) {
            if (grow_serial != want_serial) {
                want_track = grow_track;
                want_complete = false;
                want_segmented = true;
                want_limit = static_cast<u32>(value);
                want_serial = grow_serial;
            } else if (static_cast<u32>(value) > want_limit) {
                want_limit = static_cast<u32>(value);
                want_resume = true;   // P13: if it was taken as done, carry on to the new count
            }
            post_load();
        }
        grow_staged = false;
        return;
    }
    if (id < kBegin || id > kCommit) return;
    const u32 v = static_cast<u32>(value);
    if (id == kBegin) {
        staging = true;
        staged = Command{};
        staged.serial = v >> 8;
        staged.flags = v & 0xffu;
        staged_mask = 1;
        return;
    }
    if (!staging) return;
    if (id == kSource) { staged.mode = v & 0xffu; staged.track = (v >> 8) & 0xffu; staged_mask |= 2; }
    else if (id == kWhich) { staged.which = v; staged_mask |= 4; }
    else if (id == kStart) { staged.start = v; staged_mask |= 8; }
    else if (id == kEnd) { staged.end = v; staged_mask |= 16; }
    else if (id == kLevel) { staged.level = value; staged_mask |= 32; }
    else {
        if (staged_mask == 63 && v == staged.serial) {
            published_version = published_version + 1;   // odd: writing
            barrier();
            published = staged;
            barrier();
            published_version = published_version + 1;
            if (staged.serial != report_serial) {
                report_serial = staged.serial;
                log_command(staged);
            }
            // the loader: the preview named (played, landed or growing), or the buffer let go
            const bool growing = (staged.flags & kGrowing) != 0;
            if (staged.flags & kRelease) {
                want_serial = 0;
                want_release = true;
                post_load();
            } else if ((staged.mode == kFromFile || staged.mode == kLiveFile) && staged.which && staged.which != want_serial) {
                // a preview not announced by kGrow: one plain file (the quick first part)
                want_track = staged.track;
                want_complete = !growing;
                want_segmented = false;
                want_limit = 0;
                want_serial = staged.which;
                post_load();
            } else if ((staged.mode == kFromFile || staged.mode == kLiveFile) && staged.which && !growing && !want_complete) {
                want_complete = true;   // it landed: its last segment count has been sent
                post_load();
            }
        }
        staging = false;
    }
}

namespace {
// The player's block into left and right (zeroed by the caller): commands, starts, then the play.
void make_block(i32* left, i32* right) {
    Command c;
    if (take(c)) {
        player.quiet = 0;
        if (c.serial != player.serial) {
            // a new command: stop what plays (one block's fade); a play waits until it is ready
            player.serial = c.serial;
            if (player.active) player.stopping = true;
            player.waiting = (c.flags & kPlay) && c.end > c.start &&
                             (c.mode == kFromCache || c.mode == kFromFile || c.mode == kLive || c.mode == kLiveFile);
            if (player.waiting) player.next = c;
            player.wait_blocks = 0;
        } else if (c.flags & kPlay) {
            // the same play, resent: a growing preview that landed gets its real end (P8)
            Command& w = player.waiting ? player.next : player.playing;
            w.flags = c.flags;
            // only a preview's end moves (a landed render's real length); a cache play keeps the end it
            // started with, inside the sample (review)
            if (c.end > c.start && w.mode == kFromFile) w.end = c.end;
        }
    } else if (player.active || player.waiting) {
        if (++player.quiet > kTimeoutBlocks) {
            player.waiting = false;
            if (player.active) player.stopping = true;
        }
    }
    // a preview being replaced in the buffer stops at once
    if (player.active && player.from_file && load_serial != player.file_serial) player.stopping = true;
    if (player.waiting && !player.active) {
        if (start(player.next)) player.waiting = false;
        else if (++player.wait_blocks > 1400u) player.waiting = false;   // review: ~2 s of failed starts: give up
    }
    if (!player.active) return;
    if (player.live) {
        live_block(left, right);
        return;
    }
    const Command& w = player.playing;
    const bool reverse = (w.flags & kReverse) != 0;
    const u32 have = player.from_file ? loaded_frames : w.end;
    // P8: a growing preview stops cleanly where what's loaded ends (a block's fade before it)
    if (player.from_file && !reverse && player.pos + 2 * kFrames >= have && have < w.end &&
        ((w.flags & kGrowing) || load_done) && !player.stopping) {
        player.stopping = true;
        report.starved = true;
    }
    u32 peak = report.peak;
    for (u32 i = 0; i < kFrames; ++i) {
        if (player.stopping) {
            if (player.ramp <= kStopStep) { player.ramp = 0; break; }
            player.ramp -= kStopStep;
        } else if (player.ramp < 256) {
            player.ramp += kRampStep;
        }
        const u32 f = player.pos;
        const u32 g = reverse ? (f > w.start ? f - 1 : f) : (f + 1 < w.end ? f + 1 : f);
        i32 out[2] = {0, 0};
        if (f < have && g < have) {
            const i32 frac15 = static_cast<i32>(player.frac >> 1);
            for (u32 ch = 0; ch < 2; ++ch) {
                const i32 a = at(f, ch), b = at(g, ch);
                const i32 s = a + (((b - a) * frac15) >> 15);
                i32 word_out = gain_word(s, player.gain);   // pcm << 16 at 0 dB
                if (player.ramp < 256) word_out = (word_out >> 8) * static_cast<i32>(player.ramp);
                word_out = opxy::q31::multiply(word_out, kKeyGain);   // the key's level (P4)
                out[ch] = word_out;
                const u32 m = static_cast<u32>(word_out < 0 ? -word_out : word_out);
                if (m > peak) peak = m;
            }
        }   // else: not loaded yet: silence, keep time
        left[i] = saturate_add(left[i], out[0]);
        right[i] = saturate_add(right[i], out[1]);
        player.frac += player.step & 0xffffu;
        const u32 move = (player.step >> 16) + (player.frac >> 16);
        player.frac &= 0xffffu;
        ++report.frames;
        if (reverse) {
            if (player.pos < w.start + move) { player.stopping = true; player.pos = w.start; }
            else player.pos -= move;
        } else {
            player.pos += move;
            if (player.pos >= w.end) { player.stopping = true; player.pos = w.end - 1; }
        }
    }
    report.peak = peak;
    if (player.stopping && !player.ramp) {
        player.active = false;
        unpin();
    }
}
void produce() {
    for (u32 i = 0; i < kFrames; ++i) studio_l[i] = studio_r[i] = 0;
    make_block(studio_l, studio_r);
    block_made = true;
}
// P10: the block into FX I's and FX II's buses at the send amounts, eased.
void feed_sends() {
    for (u32 s = 0; s < 2; ++s) {
        const i32 target = send_gain(s, send_amount[s]);
        const i32 d = target - send_now[s];
        send_now[s] += d > kSendEase ? kSendEase : d < -kSendEase ? -kSendEase : d;
        if (!send_now[s] || !player.active) continue;
        i32* bus = host::fx_bus(s);
        for (u32 i = 0; i < kFrames; ++i) {
            bus[2 * i] = saturate_add(bus[2 * i], mul31(studio_l[i], send_now[s]));
            bus[2 * i + 1] = saturate_add(bus[2 * i + 1], mul31(studio_r[i], send_now[s]));
        }
    }
}
}  // namespace

// P11: the studio's dry block plus both send returns, back at the file's level, into the capture.
namespace {
i16 to16(i32 word) {
    // word is pcm << 7 x 0.069 (P4): x 7447 (about 2^31 / kKeyGain) then >> 16, rounded, held in range
    if (word > 288358) return 32767;
    if (word < -288358) return -32768;
    const i32 v = (word * 7447 + 32768) >> 16;
    return static_cast<i16>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}
// P15 (editor.cpp A47): a "full" tail records on after the play ends until the print stays under
// kQuietLevel (-60 dBFS) for kQuietHold (2 s), keeping kQuietKeep (0.1 s) past its last louder frame; the
// size the main board sent (the edit plus 30 s) is the cap for feedback that never dies away.
const i32 kQuietLevel = 32;
const u32 kQuietHold = 2 * 44100, kQuietKeep = 4410;
void capture_block() {
    i16* const out = bounce_pcm;
    if (!out) return;
    const i32* const fx0 = host::fx_bus(0);
    const i32* const fx1 = host::fx_bus(1);
    u32 at = bounce_at;
    if (bounce_until_quiet && !bounce_play_ended && !player.active) {
        bounce_play_ended = true;   // P15: from here the quiet counts
        bounce_loud_at = at;
    }
    const bool listen = bounce_until_quiet && bounce_play_ended;
    for (u32 i = 0; i < kFrames && at < bounce_total; ++i, ++at) {
        const i32 l = saturate_add(saturate_add(studio_l[i], fx0[2 * i]), fx1[2 * i]);
        const i32 r = saturate_add(saturate_add(studio_r[i], fx0[2 * i + 1]), fx1[2 * i + 1]);
        out[2 * at] = to16(l);
        out[2 * at + 1] = to16(r);
        if (listen) {
            const i32 a = out[2 * at], b = out[2 * at + 1];
            if (a > kQuietLevel || a < -kQuietLevel || b > kQuietLevel || b < -kQuietLevel) bounce_loud_at = at;
        }
    }
    if (listen && at < bounce_total && at - bounce_loud_at >= kQuietHold) {
        const u32 keep = bounce_loud_at + kQuietKeep;
        bounce_total = keep < at ? keep : at;   // P15: quiet: the file ends here (the writer reads this size)
    }
    bounce_at = at;
    if (at >= bounce_total) {
        bounce_capturing = false;
        barrier();
        bounce_full = true;   // the main board's next poke starts the writer
    }
}
}  // namespace

// P10: drum_send.cpp's hook before each track's voice loop (8d510bea): the first of a block makes
// the player's block and feeds the sends, before stock runs the send effects.
void mixer_begin() {
    if (block_made) return;
    produce();
    feed_sends();
}

void master(i32* left, i32* right) {   // after stock's master EQ (the SDK runs it first)
    if (!block_made) produce();   // no mixer hook ran this block: the dry only
    for (u32 i = 0; i < kFrames; ++i) {
        left[i] = saturate_add(left[i], studio_l[i]);
        right[i] = saturate_add(right[i], studio_r[i]);
    }
    if (bounce_capturing) capture_block();
    block_made = false;
    ++blocks;
    if (hold_age <= kTimeoutBlocks) hold_age = hold_age + 1;
}

// P7: drum_send.cpp's voice hook (8d51c7b2), each voice's finished block before stock adds it
// into its track's sum. Silences the held track's voices: zero while held, a one-block ramp
// down when the hold starts and up when it ends.
void voice(i32* out, u32 track) {
    if (voice_block != blocks) {
        voice_block = blocks;
        mute_was = mute_now;
        mute_now = hold_age <= kTimeoutBlocks ? hold_value : 0;
    }
    const bool was = mute_was == track + 1, now = mute_now == track + 1;
    if (!was && !now) return;
    if (was && now) {
        u32 peak = report.voice_peak;
        for (u32 i = 0; i < 2 * kFrames; ++i) {
            const u32 m = static_cast<u32>(out[i] < 0 ? -out[i] : out[i]);
            if (m > peak) peak = m;
            out[i] = 0;
        }
        report.voice_peak = peak;
        return;
    }
    // 64 frames: (x >> 6) * 63 stays inside 31 bits
    for (u32 i = 0; i < kFrames; ++i) {
        const i32 g = static_cast<i32>(now ? kFrames - 1 - i : i);
        out[2 * i] = (out[2 * i] >> 6) * g;
        out[2 * i + 1] = (out[2 * i + 1] >> 6) * g;
    }
}

// P4: the gain stock gives the held track (drum_send.cpp's 8d510ea8 hook: where the track's gain
// ramp ended) and its dry level (the 8d511116 hook: [FP-0x68]), for the log.
void gain(u32 track, i32 gain) {
    if (mute_now == track + 1) track_gain = gain;
}
void level(u32 track, i32 level) {
    if (mute_now == track + 1) track_level = level;
}
}}}}  // namespace opxy::extensions::sample_studio::play
