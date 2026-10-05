// PS5: the PS3 system software's installation, without Qt.
//
// The desktop installs it from main_window::HandlePupInstallation (rpcs3qt):
// the PUP's update database (file 0x300) is a TAR of dev_flash_* packages,
// each an SCE-encrypted TAR that goes to /dev_flash. These are the same steps,
// with RPCS3's own PUP, TAR and SCE readers; what the desktop shows in dialogs
// goes to the log (and the title's trace). The PUP is Sony's own download.

#include "stdafx.h"
#include "ps5_firmware.h"

#include "Utilities/File.h"
#include "Emu/System.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "Crypto/key_vault.h"
#include "util/sysinfo.hpp"

#include <algorithm>

LOG_CHANNEL(fw_log, "FW");

namespace
{
	// The version line a PUP carries (file 0x100), e.g. "4.93"
	std::string pup_version(const pup_object& pup)
	{
		std::string version;
		if (fs::file file = pup.get_file(0x100))
		{
			version = file.to_string();
		}
		if (const usz end = version.find('\n'); end != umax)
		{
			version.erase(end);
		}
		return version;
	}
}

ps5_firmware_result ps5_install_firmware(const std::string& pup_path)
{
	fs::file pup_file(pup_path);
	if (!pup_file)
	{
		return ps5_firmware_result::no_file;
	}

	pup_object pup(std::move(pup_file));
	if (pup.operator pup_error() != pup_error::ok)
	{
		fw_log.error("%s is not a valid PS3 update file: %s", pup_path, pup.get_formatted_error());
		return ps5_firmware_result::failed;
	}

	const std::string version = pup_version(pup);
	if (version.empty())
	{
		fw_log.error("%s carries no version", pup_path);
		return ps5_firmware_result::failed;
	}

	if (const std::string installed = utils::get_firmware_version(); installed == version)
	{
		fw_log.notice("PS3 system software %s is installed already", installed);
		return ps5_firmware_result::already_installed;
	}

	fs::file update_files_f = pup.get_file(0x300);
	if (!update_files_f || !update_files_f.size())
	{
		fw_log.error("%s has no installation packages", pup_path);
		return ps5_firmware_result::failed;
	}

	tar_object update_files(update_files_f);
	auto packages = update_files.get_filenames();
	packages.erase(std::remove_if(packages.begin(), packages.end(), [](const std::string& s) { return s.find("dev_flash_") == umax; }), packages.end());
	if (packages.empty())
	{
		fw_log.error("%s has no dev_flash packages", pup_path);
		return ps5_firmware_result::failed;
	}

	fw_log.success("Installing PS3 system software %s: %d packages into %s", version, packages.size(), g_cfg_vfs.get_dev_flash());

	// tar_object::extract writes under /dev_flash
	vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());

	usz done = 0;
	for (const auto& package : packages)
	{
		auto stream = update_files.get_file(package);
		if (stream->m_file_handler)
		{
			// Read all the data in
			stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
		}

		fs::file package_file = fs::make_stream(std::move(stream->data));

		SCEDecrypter decrypter(package_file);
		decrypter.LoadHeaders();
		decrypter.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
		decrypter.DecryptData();

		auto contents = decrypter.MakeFile();
		if (contents.size() < 3)
		{
			fw_log.error("Package %s could not be decrypted", package);
			return ps5_firmware_result::failed;
		}

		tar_object package_tar(contents[2]);
		if (!package_tar.extract())
		{
			fw_log.error("Package %s could not be extracted", package);
			return ps5_firmware_result::failed;
		}

		done++;
		fw_log.notice("Installed %s (%d of %d)", package, done, packages.size());
	}

	update_files_f.close();

	// Mounts again, with the new /dev_flash
	Emu.Init();

	const std::string installed = utils::get_firmware_version();
	if (installed != version)
	{
		fw_log.error("Installed %s, but the system software reads '%s'", version, installed);
		return ps5_firmware_result::failed;
	}

	fw_log.success("Installed PS3 system software %s", installed);
	return ps5_firmware_result::installed;
}
