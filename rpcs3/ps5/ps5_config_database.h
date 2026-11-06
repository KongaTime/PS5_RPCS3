#pragma once

// PS5: RPCS3's database of recommended settings, as the title carries it
// (ps5_config_database.inc, from api.rpcs3.net by config_database.py).
//
// A game's recommended settings lie under its own: the global config, then
// the game's entry here (with Recommended settings on, the default), then
// its custom config (ps5_ui.cpp's game settings, Emulator::Load).

#include <string_view>

// The game's entry, as YAML over the global config; empty for a game the
// database does not have
std::string_view ps5_config_database_entry(std::string_view serial);
