// The harness's stand-in for the platform's barrier.hpp: one thread, no barrier needed.
#pragma once
namespace opxy { namespace platform {
inline void barrier() { __atomic_signal_fence(__ATOMIC_SEQ_CST); }
}}
