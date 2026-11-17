// The harness's stand-in for the SDK's Sample Studio services (opxy/sample_studio.hpp): enough for
// the editor to draw a frame on Linux. Nothing here touches a file, the audio board or stock.
#include <opxy/sample_studio.hpp>

namespace harness {
bool shift;
}

namespace opxy { namespace studio {
u32 now_ms() { return 0; }
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
bool library_on_top() { return true; }
bool library_busy() { return false; }
u32 selected_track() { return 0; }
u32 sample_engine(u32) { return engine_sampler; }
u32 selected_key(u32) { return 60; }
bool shift_held() { return harness::shift; }
u32 project_bpm_x10() { return 1200; }
u32 track_engine(u32) { return 0; }
bool highlighted(char* out, u32 size) {
    if (size) out[0] = 0;
    return false;
}
bool highlight_seen() { return false; }
void stop_audition() {}
void relist_library() {}
void rescan_presets() {}
u32 note_serial(u32) { return 0; }
bool note_held(u32, u32) { return false; }
unsigned char* key_entry(u32, u32) { return nullptr; }
const char* entry_path(const unsigned char*) { return ""; }
i32 entry_frames(const unsigned char*) { return 0; }
SampleRegion entry_region(const unsigned char*) { return SampleRegion(); }
i32 entry_level(const unsigned char*) { return 0; }
bool entry_reversed(const unsigned char*) { return false; }
u32 entry_root(const unsigned char*) { return 60; }
i32 entry_fine(const unsigned char*) { return 0; }
void set_entry_tuning(unsigned char*, u32, i32) {}
void set_entry_whole(unsigned char*, u32) {}
bool put_file(u32, u32, u32, const char*, u32, u32) { return false; }
void send_regions(u32, bool) {}
void samples_changed() {}
bool samples_gathered() { return true; }
u32 sample_key(const char*) { return 0; }
Borrowed borrow(u32) { return Borrowed{nullptr, 0, false}; }
void release_sample(u32) {}
SampleView sample_view(const void*) { return SampleView(); }
void send(u32, i32) {}
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
