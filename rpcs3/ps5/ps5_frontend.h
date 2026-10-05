#pragma once

// PS5: RPCS3's frontend on the console, in place of the Qt one.
//
// The title (PS5_RPCS3Title) calls rpcs3_ps5_run from its main thread after its
// own platform layer is up (klog, the pad, the shell's splash) and volk points
// at the RADV it links. RPCS3's files live under /app0/rpcs3/.
//
// What the title provides, so this frontend does not depend on its files:
// - the pads, read through rpcs3_ps5_title::poll_pads (below);
// - catchReturnFromMain(status), which asks the shell to close the title and
//   never returns: a fatal error ends through it (a title never calls exit).

#include <cstdint>

// One player's controller as the title reads it
struct rpcs3_ps5_pad
{
	bool connected;
	std::uint32_t buttons; // rpcs3_ps5_button bits
	float left_x, left_y, right_x, right_y; // -1..1, y down-positive
	float l2, r2; // 0..1
};

enum rpcs3_ps5_button : std::uint32_t
{
	RPCS3_PS5_UP = 1u << 0,
	RPCS3_PS5_DOWN = 1u << 1,
	RPCS3_PS5_LEFT = 1u << 2,
	RPCS3_PS5_RIGHT = 1u << 3,
	RPCS3_PS5_CROSS = 1u << 4,
	RPCS3_PS5_CIRCLE = 1u << 5,
	RPCS3_PS5_SQUARE = 1u << 6,
	RPCS3_PS5_TRIANGLE = 1u << 7,
	RPCS3_PS5_L1 = 1u << 8,
	RPCS3_PS5_R1 = 1u << 9,
	RPCS3_PS5_L3 = 1u << 10,
	RPCS3_PS5_R3 = 1u << 11,
	RPCS3_PS5_START = 1u << 12,  // the PS5's OPTIONS
	RPCS3_PS5_SELECT = 1u << 13, // the touch pad's click
	RPCS3_PS5_PS = 1u << 14,     // the PS button belongs to the console's shell
};

constexpr int rpcs3_ps5_pad_players = 4;

struct rpcs3_ps5_title
{
	// Reads every player's controller; called by RPCS3's pad thread, about
	// once a millisecond, never by another thread
	void (*poll_pads)(rpcs3_ps5_pad pads[rpcs3_ps5_pad_players]);
};

// boot_path: what to boot (an ELF, or a game's folder); empty to start the
// emulator, report the firmware it finds, and stop.
//
// Returns 0 when RPCS3 started and stopped cleanly, else 1.
int rpcs3_ps5_run(const char* boot_path, const rpcs3_ps5_title& title);
