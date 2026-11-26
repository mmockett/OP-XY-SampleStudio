// The harness's stand-in for the SDK's Sample Studio services (opxy/sample_studio.hpp): enough for
// the editor to draw a frame on Linux. Nothing here touches a file, the audio board or stock.
#include <opxy/sample_studio.hpp>

namespace harness {
bool shift;
opxy::u32 clock;                                         // now_ms()
opxy::u32 engine = opxy::studio::engine_sampler;          // the selected track's sample engine
const char* highlight = "";                              // the library's highlighted file ("" for none)
bool highlight_seen;                                     // stock's record of the last highlighted file
opxy::u32 highlight_asks;                                // highlighted() calls: stock's library tree
opxy::u32 notes;                                         // the track's note serial
opxy::i32 sent[15];                                      // the last value sent on each channel
bool sent_on[15];
// A drum kit's region entries by place, keys 53..76: an entry exists where `has` says so, and
// put_file makes one on an empty key as stock's library does for an empty pad.
unsigned char kit[24][opxy::studio::entry_bytes];
bool has[24];
opxy::u32 put[24];                                       // put_file calls per key
int alias[24];                                           // a key on another place's region (-1: none)
int fold_into = -1;                                      // put_file on an empty key lands in this place's region
opxy::u32 relists, rescans;                              // the library listed again, the preset index built again
bool off_top;                                            // another browser (presets) on the shared screen
bool drum;                                               // the track is a drum sampler, Drumline or a synthesis kit
opxy::u32 held_note = 128;                               // the note held on the track (128: none)
}

namespace opxy { namespace studio {
u32 now_ms() { return harness::clock; }
u32 now_us() { return 0; }
u32 cycles() { return 0; }
void barrier() {}
void log(u32, const char*, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32, i32) {}
void log_audio(u32, const char*, i32, i32, i32, i32, i32, i32) {}
void post(void (*)(u32), void (*)(u32), u32) {}
File* file_open(const char*, const char*) { return nullptr; }
u32 file_read(void*, u32, u32, File*) { return 0; }
u32 file_write(const void*, u32, u32, File*) { return 0; }
void file_close(File*) {}
bool rename_file(const char*, const char*) { return false; }
void remove_file(const char*) {}
void make_folder(const char*) {}
bool file_exists(const char*) { return false; }
void* allocate(u32) { return nullptr; }
void release(void*) {}
void popup(const char*) {}
bool library_on_top() { return !harness::off_top; }
bool library_busy() { return false; }
u32 selected_track() { return 0; }
u32 sample_engine(u32) { return harness::engine; }
bool drum_track(u32) { return harness::drum; }
u32 selected_key(u32) { return 60; }
bool shift_held() { return harness::shift; }
u32 project_bpm_x10() { return 1200; }
u32 track_engine(u32) { return 0; }
bool highlighted(char* out, u32 size) {
    ++harness::highlight_asks;
    if (!size) return false;
    u32 n = 0;
    for (; harness::highlight[n] && n + 1 < size; ++n) out[n] = harness::highlight[n];
    out[n] = 0;
    return n > 0;
}
bool highlight_seen() { return harness::highlight_seen; }
void stop_audition() {}
void relist_library() { ++harness::relists; }
void rescan_presets() { ++harness::rescans; }
u32 note_serial(u32) { return harness::notes; }
bool note_held(u32, u32 note) { return note == harness::held_note; }
unsigned char* key_entry(u32, u32 key) {
    if (key < 53 || key >= 77) return nullptr;
    if (harness::alias[key - 53] >= 0) return harness::kit[harness::alias[key - 53]];
    return harness::has[key - 53] ? harness::kit[key - 53] : nullptr;
}
const char* entry_path(const unsigned char* entry) { return reinterpret_cast<const char*>(entry + 0x20); }
i32 entry_frames(const unsigned char*) { return 0; }
SampleRegion entry_region(const unsigned char*) { return SampleRegion(); }
i32 entry_level(const unsigned char*) { return 0; }
bool entry_reversed(const unsigned char*) { return false; }
// The pad record's root and fine bytes (stock_sample.hpp pad_record: kRoot 24, kFine 28).
u32 entry_root(const unsigned char* entry) { return entry ? entry[24] : 60; }
i32 entry_fine(const unsigned char* entry) { return entry ? static_cast<signed char>(entry[28]) : 0; }
void set_entry_tuning(unsigned char* entry, u32 root, i32 fine) {
    entry[24] = static_cast<unsigned char>(root);
    entry[28] = static_cast<unsigned char>(fine);
}
void set_entry_whole(unsigned char*, u32) {}
bool put_file(u32, u32, u32 key, const char* path, u32, u32) {
    if (key < 53 || key >= 77) return false;
    ++harness::put[key - 53];
    u32 place = key - 53;
    if (harness::alias[place] >= 0) place = static_cast<u32>(harness::alias[place]);
    else if (!harness::has[place] && harness::fold_into >= 0) place = static_cast<u32>(harness::alias[key - 53] = harness::fold_into);
    else harness::has[place] = true;
    char* name = reinterpret_cast<char*>(harness::kit[place] + 0x20);
    u32 n = 0;
    for (; path[n] && n + 1 < path_bytes; ++n) name[n] = path[n];
    name[n] = 0;
    return true;
}
void send_regions(u32, bool) {}
void samples_changed() {}
bool samples_gathered() { return true; }
u32 sample_key(const char*) { return 0; }
Borrowed borrow(u32) { return Borrowed{nullptr, 0, false}; }
void release_sample(u32) {}
SampleView sample_view(const void*) { return SampleView(); }
void send(u32 channel, i32 value) {
    if (channel < 15) {
        harness::sent[channel] = value;
        harness::sent_on[channel] = true;
    }
}
bool surface_ready(const char* const*, u32, u32, u32, u32) { return true; }
void surface_present(const Surface&) {}
void surface_blank() {}
void stock_frame() {}
void* pool_allocate(u32) { return nullptr; }
void pool_free(void*) {}
u32 pool_in_use() { return 0; }
const void* pin(u32) { return nullptr; }
void unpin(u32) {}
i32* fx_bus(u32) { return nullptr; }
}}  // namespace opxy::studio
