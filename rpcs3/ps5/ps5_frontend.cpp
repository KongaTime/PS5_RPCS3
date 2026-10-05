// PS5: RPCS3's frontend on the console (ps5_frontend.h).
//
// The emulator's callbacks follow the headless frontend (headless_application.cpp)
// and main_application.cpp's, without Qt: the Vulkan renderer drawing to the
// display (ps5_gs_frame), the null audio, keyboard, mouse, camera and music
// handlers until the console's own are written, no dialogs. The main thread
// runs a queue of the calls RPCS3 makes "from the main thread".

#include "stdafx.h"
#include "ps5_frontend.h"
#include "ps5_gs_frame.h"
#include "ps5_pad_handler.h"
#include "Input/pad_thread.h"

#include "util/logs.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/Thread.h"
#include "Utilities/File.h"
#include "Emu/emu_callbacks.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/IdManager.h"
#include "Emu/Io/pad_config.h"
#include "Emu/Io/KeyboardHandler.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/Audio/Null/null_enumerator.h"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/VK/VKGSRender.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Cell/Modules/sceNp.h"
#include "util/video_source.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

LOG_CHANNEL(sys_log, "SYS");
LOG_CHANNEL(ps5_log, "PS5");

namespace
{
	// The console's display, as swapchain_ps5.hpp chooses it
	constexpr int display_width = 3840;
	constexpr int display_height = 2160;
	constexpr f64 display_rate = 59.94;

	// RPCS3's log to klog: the title's standard error reaches klog, prefixed
	// with its name. Warnings and worse, and what RPCS3 always logs.
	struct klog_listener final : logs::listener
	{
		void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix, std::string_view text) override
		{
			if (msg > logs::level::warning)
			{
				return;
			}
			std::fprintf(stderr, "%.*s%s: %.*s\n", static_cast<int>(prefix.size()), prefix.data(),
				msg->name, static_cast<int>(text.size()), text.data());
		}
	};

	// The calls RPCS3 makes on the main thread
	struct main_queue
	{
		std::mutex mutex;
		std::condition_variable cv;
		std::deque<std::pair<std::function<void()>, atomic_t<u32>*>> calls;
		bool quit = false;

		void post(std::function<void()> func, atomic_t<u32>* wake_up)
		{
			{
				std::lock_guard lock(mutex);
				calls.emplace_back(std::move(func), wake_up);
			}
			cv.notify_one();
		}

		void request_quit()
		{
			{
				std::lock_guard lock(mutex);
				quit = true;
			}
			cv.notify_one();
		}

		// Runs the calls waiting now, without waiting for more
		void run_pending()
		{
			std::unique_lock lock(mutex);
			while (!calls.empty())
			{
				auto [func, wake_up] = std::move(calls.front());
				calls.pop_front();
				lock.unlock();
				func();
				if (wake_up)
				{
					*wake_up = true;
					wake_up->notify_one();
				}
				lock.lock();
			}
		}

		// Runs the calls as they come, until quit is asked for
		void run()
		{
			std::unique_lock lock(mutex);
			while (true)
			{
				cv.wait(lock, [this] { return quit || !calls.empty(); });
				while (!calls.empty())
				{
					auto [func, wake_up] = std::move(calls.front());
					calls.pop_front();
					lock.unlock();
					func();
					if (wake_up)
					{
						*wake_up = true;
						wake_up->notify_one();
					}
					lock.lock();
				}
				if (quit)
				{
					return;
				}
			}
		}
	};

	main_queue g_main;

	void create_callbacks()
	{
		g_emu_callbacks.call_from_main_thread = [](std::function<void()> func, atomic_t<u32>* wake_up)
		{
			g_main.post(std::move(func), wake_up);
		};

		g_emu_callbacks.try_to_quit = [](bool force_quit, std::function<void()> on_exit) -> bool
		{
			if (!force_quit)
			{
				return false;
			}
			if (on_exit)
			{
				on_exit();
			}
			g_main.request_quit();
			return true;
		};

		g_emu_callbacks.update_emu_settings = []()
		{
			Emu.CallFromMainThread([]() { rpcs3::utils::configure_logs(Emu.IsStopped()); });
		};
		g_emu_callbacks.save_emu_settings = []()
		{
			Emu.BlockingCallFromMainThread([]() { Emulator::SaveSettings(g_cfg.to_string(), Emu.GetTitleID()); });
		};

		g_emu_callbacks.init_kb_handler = []()
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_mouse_handler = []()
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_pad_handler = [](std::string_view title_id)
		{
			// pad_thread gives players 1 to 4 the console's controllers (ps5_pad_handler)
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
		};

		g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
		{
			// The console's audio output is the next step
			return std::make_shared<NullAudioBackend>();
		};
		g_emu_callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator>
		{
			return std::make_shared<null_enumerator>();
		};

		g_emu_callbacks.init_gs_render = [](utils::serial* ar)
		{
			switch (const video_renderer type = g_cfg.video.renderer)
			{
			case video_renderer::null:
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(ar);
				break;
			case video_renderer::vulkan:
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(ar);
				break;
			default:
				fmt::throw_exception("The PS5 draws with Vulkan (or the null renderer), not %s", type);
			}
		};
		g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
		{
			return std::make_unique<ps5_gs_frame>(display_width, display_height, display_rate);
		};
		g_emu_callbacks.close_gs_frame = []() {};

		g_emu_callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return std::make_shared<null_camera_handler>(); };
		g_emu_callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base> { return std::make_shared<null_music_handler>(); };

		g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
		g_emu_callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
		g_emu_callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return {}; };
		g_emu_callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
		g_emu_callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
		g_emu_callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return {}; };

		g_emu_callbacks.on_run = [](bool) {};
		g_emu_callbacks.on_pause = []() {};
		g_emu_callbacks.on_resume = []() {};
		g_emu_callbacks.on_stop = []() {};
		g_emu_callbacks.on_ready = []() {};
		g_emu_callbacks.on_missing_fw = []()
		{
			ps5_log.error("No PS3 system software: install PS3UPDAT.PUP (from Sony) first");
		};
		g_emu_callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> closed_successfully, int)
		{
			if (!closed_successfully || !*closed_successfully)
			{
				ps5_log.fatal("Stopping the emulator took too long: a thread has probably deadlocked");
			}
		};
		g_emu_callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		g_emu_callbacks.enable_disc_eject = [](bool) {};
		g_emu_callbacks.enable_disc_insert = [](bool) {};
		g_emu_callbacks.handle_taskbar_progress = [](s32, s32) {};

		g_emu_callbacks.get_localized_string = [](localized_string_id, const char*) -> std::string { return {}; };
		g_emu_callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return {}; };
		g_emu_callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return {}; };
		g_emu_callbacks.get_photo_path = [](std::string_view title) -> std::string
		{
			return fs::get_config_dir() + "photos/" + std::string(title) + "/";
		};
		g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		g_emu_callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
		g_emu_callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
		g_emu_callbacks.get_font_dirs = []() { return std::vector<std::string>{}; };
		g_emu_callbacks.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
		g_emu_callbacks.add_breakpoint = [](u32) {};
		g_emu_callbacks.display_sleep_control_supported = []() { return false; };
		g_emu_callbacks.enable_display_sleep = [](bool) {};
		g_emu_callbacks.check_microphone_permissions = []() {};
		g_emu_callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return {}; };
		g_emu_callbacks.enable_gamemode = [](bool) {};
		g_emu_callbacks.get_database_config = [](const std::string&) -> std::string { return {}; };
	}
}

// What the Qt frontend defines for the emulator, without Qt

// The input configurations (rpcs3qt/pad_settings_dialog.cpp on the desktop)
cfg_input_configurations g_cfg_input_configs;

// The desktop's --input-config option (rpcs3.cpp): none on the console
std::string g_input_config_override;

// Repeats an operation until it succeeds, keeping the main thread's calls
// running in between when it is the main thread that waits
void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	while (!wrapped_op())
	{
		if (thread_ctrl::is_main())
		{
			g_main.run_pending();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(std::max(repeat_duration_ms, 1)));
	}
}

// The title's: asks the shell to close it, and never returns (ps5_frontend.h)
extern "C" void catchReturnFromMain(int status);

// A fatal error ends the title through the shell: a title never calls exit
[[noreturn]] void report_fatal_error(std::string_view text, bool /*is_html*/, bool /*include_help_text*/)
{
	std::fprintf(stderr, "RPCS3: fatal error: %.*s\n", static_cast<int>(text.size()), text.data());
	logs::listener::sync_all();
	catchReturnFromMain(1);
	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
}

int rpcs3_ps5_run(const char* boot_path, const rpcs3_ps5_title& title)
{
	ps5_pad_handler::set_source(title.poll_pads);

	// RPCS3's configuration, caches, dev_hdd0 and log go to /app0/rpcs3/ (fs::get_config_dir
	// and fs::get_cache_dir read these before anything touches the filesystem)
	::setenv("XDG_CONFIG_HOME", "/app0", 1);
	::setenv("XDG_CACHE_HOME", "/app0", 1);

	if (!thread_ctrl::is_main())
	{
		std::fprintf(stderr, "rpcs3_ps5_run: not on the main thread\n");
		return 1;
	}

	// The thread pool's finalizer, on first use (as rpcs3.cpp)
	static_cast<void>(named_thread("", [](int) {}));

	// Listeners stay in RPCS3's list for the life of the process
	static klog_listener klog;
	logs::listener::add(&klog);
	std::unique_ptr<logs::listener> log_file = logs::make_file_listener(fs::get_cache_dir() + "RPCS3.log", 256ull * 1024 * 1024);
	{
		logs::stored_message ver{sys_log.always()};
		ver.text = fmt::format("RPCS3 for the PS5, on %s", utils::get_system_info());
		logs::set_init({std::move(ver)});
	}

	create_callbacks();

	Emu.SetHasGui(false);
	Emu.SetHeadless(false);
	Emu.SetUsr("00000001");
	Emu.Init();

	const std::string firmware = utils::get_firmware_version();
	sys_log.always()("PS3 system software: %s", firmware.empty() ? "missing" : firmware);
	rpcs3::utils::configure_logs(true);

	int status = 0;
	if (boot_path && *boot_path)
	{
		if (const game_boot_result result = Emu.BootGame(boot_path, "", true); result != game_boot_result::no_errors)
		{
			sys_log.error("Booting %s failed: %s", boot_path, result);
			status = 1;
		}
		else
		{
			// Until the game stops and RPCS3 asks to quit
			g_emu_callbacks.on_stop = []() { g_main.request_quit(); };
			g_main.run();
		}
	}

	if (!Emu.IsStopped())
	{
		Emu.Kill(false);
	}

	logs::listener::sync_all();
	logs::listener::shutdown_all();
	return status;
}
