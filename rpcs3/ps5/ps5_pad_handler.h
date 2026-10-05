#pragma once

// PS5: the console's controllers as the PS3's pads. The title reads them (its
// pad API, through rpcs3_ps5_title::poll_pads) and this handler gives players
// 1 to 4 what it read; it takes the DualSense handler's place in pad_thread.

#include "Emu/Io/PadHandler.h"
#include "ps5_frontend.h"

class ps5_pad_handler final : public PadHandlerBase
{
public:
	ps5_pad_handler();

	// The title's pad reader, set once by rpcs3_ps5_run
	static void set_source(void (*poll_pads)(rpcs3_ps5_pad pads[rpcs3_ps5_pad_players]));

	void init_config(cfg_pad* cfg) override;
	std::vector<pad_list_entry> list_devices() override;
	bool bindPadToDevice(std::shared_ptr<Pad> pad) override;
	void process() override;

private:
	std::vector<std::shared_ptr<Pad>> m_pads; // by player
	std::array<bool, rpcs3_ps5_pad_players> m_connected{};
};
