#pragma once

// PS5: the console's CPUs as a title gets them, and one core kept for one busy
// SPU thread.
//
// A title's threads run on thirteen of the console's sixteen CPU threads
// (PS5_PayloadSDK's platform/docs/PROBE.md), two to a core but the last.
// Which two share a core is measured once, at the title's start: a busy loop
// beside its SMT sibling keeps about half its speed, beside any other CPU
// nearly all of it.
//
// With the CPU tab's "Own core for the busy SPU" on, the kernel thread of a
// SPURS instance of one SPU (GTA IV's secondary instance, the busiest thread
// in its busy streets: ps5_frontend.cpp's status lines) runs alone on one CPU
// of a core, and every other thread of the emulator keeps off both of that
// core's CPUs (Utilities/Thread.cpp).

#include "util/types.hpp"

#include <string>
#include <string_view>

// Measures which of the title's CPUs share a core; returns what it found, for the trace
std::string ps5_measure_cpus();

// The CPUs every new emulator thread keeps off, while the setting is on: 0 for none
u64 ps5_cpu_kept_mask();

// The CPU the calling SPU kernel thread (named `thread`) is to run alone on:
// once for each emulation, -1 when the setting is off or the core is taken
s32 ps5_cpu_take_own_core(std::string_view thread);

// With /app0/rpcs3-spu-spread.txt there: the CPU the calling SPU thread is to
// stay on, one CPU a thread, a core each first (the CPUs 0 and 1 left to the
// PPU, the RSX and the rest), the CPU given back when the thread ends; once
// for each thread, -1 after the first call or without the file. The kernel
// alone put three of Army of Two: The 40th Day's four busy SPUs on CPU 1
// (build 176: 79%, 47% and 34% of their time)
s32 ps5_cpu_spread_spu();

// The setting's state for the status lines: off, or the core and its SPU,
// and the spread SPUs' CPUs
std::string ps5_cpu_own_core_text();

// The CPU the calling thread runs on now
s32 ps5_current_cpu();

// Whether a thread put on one CPU stays there, by each of the console's ways
// of putting it there: what each did, for the trace
std::string ps5_test_pinning();
