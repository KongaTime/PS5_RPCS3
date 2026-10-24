// No Wiimotes on the console: RPCS3's wiimote_handler reads them through
// hidapi, which the title has not. GunCon3 asks for them (its Wiimote
// mapping); with no handler it reads the GunCon as before
#include "stdafx.h"
#include "Input/wiimote_handler.h"

wiimote_handler* wiimote_handler::get_instance()
{
	return nullptr;
}

std::vector<wiimote_state> wiimote_handler::get_states()
{
	return {};
}
