#pragma once

// Thread placement helpers.

namespace adsb {

// Pins the calling thread to one CPU. Returns false if unsupported (macOS has
// no hard affinity) or if the call failed (e.g. CPU offline or not allowed).
bool pin_current_thread(int cpu) noexcept;

// Names the calling thread for debuggers, `top -H`, and perf (best effort).
void set_current_thread_name(const char* name) noexcept;

}  // namespace adsb
