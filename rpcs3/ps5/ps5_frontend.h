#pragma once

// PS5: RPCS3's frontend on the console, in place of the Qt one.
//
// The title (PS5_RPCS3Title) calls this from its main thread after its own
// platform layer is up (klog, the pad, the shell's splash) and volk points at
// the RADV it links. RPCS3's files live under /app0/rpcs3/.
//
// boot_path: what to boot (an ELF, or a game's folder); empty to start the
// emulator, report the firmware it finds, and stop.
//
// Returns 0 when RPCS3 started and stopped cleanly, else 1. It never exits the
// process: the title ends through the shell.
int rpcs3_ps5_run(const char* boot_path);
