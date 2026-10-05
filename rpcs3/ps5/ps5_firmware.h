#pragma once

// PS5: installs the PS3 system software from Sony's PS3UPDAT.PUP into RPCS3's
// dev_flash (ps5_firmware.cpp), as the desktop's Install Firmware does.

#include <string>

enum class ps5_firmware_result
{
	no_file,           // no PUP at the path
	already_installed, // that version is installed
	installed,
	failed,            // reported in the log
};

// Call after Emu.Init (g_cfg_vfs knows dev_flash); calls Emu.Init again after installing
ps5_firmware_result ps5_install_firmware(const std::string& pup_path);
