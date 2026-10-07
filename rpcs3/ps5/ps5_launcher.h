#pragma once

// PS5: the title's game launcher, drawn by RSX's native overlays in place of
// Big Picture Mode's dialog (Emulator::BootBigPictureMode runs it).
//
// Home: the selected game's own art (PS3_GAME/PIC1.PNG) behind its name and
// Play now / Game settings, over a row of the games' ICON0.PNG tiles.
// Library: Big Picture Mode's game grid. Settings: RPCS3's settings pages.
// L1 / R1 change tabs.

#include "Emu/RSX/Overlays/overlays.h"
#include "Emu/RSX/Overlays/BigPicture/overlay_big_picture_game_info.h"
#include "Emu/game_enumeration.h"

#include <memory>
#include <mutex>
#include <vector>

namespace rsx::overlays
{
	struct home_menu_page;

	struct ps5_launcher_game
	{
		big_picture_game_info info;
		std::unique_ptr<image_info> icon;       // ICON0.PNG
		std::unique_ptr<image_info> background; // PIC1.PNG, if the game has one
	};

	struct ps5_launcher_dialog : public user_interface
	{
		ps5_launcher_dialog();
		~ps5_launcher_dialog() override;

		void update(u64 timestamp_us) override;
		void on_button_pressed(pad_button button_press, bool is_auto_repeat) override;
		compiled_resource get_compiled() override;

		void show();

	private:
		enum class tab : u8
		{
			home,
			library,
			settings
		};

		void start_reload();
		void select_game(s32 index);
		void set_tab(tab next);
		void boot_selected();

		void build_static();
		void layout_tabs();
		void layout_home();

		std::mutex m_mutex;
		std::unique_ptr<named_thread<std::function<void()>>> m_enumeration_thread;
		game_enumeration<big_picture_game_info> m_enumeration;
		std::vector<ps5_launcher_game> m_games;
		atomic_t<bool> m_loading = true;

		tab m_tab = tab::home;
		s32 m_selected = 0;
		s32 m_first_visible = 0;

		// The page shown on the Library and Settings tabs
		std::shared_ptr<home_menu_page> m_library;
		std::shared_ptr<home_menu_page> m_settings;

		// Background: the selected game's art, a dark wash, and fades to the left and bottom
		image_view m_background;
		overlay_element m_wash;
		std::vector<std::unique_ptr<overlay_element>> m_fades;

		// Top bar
		label m_logo;
		std::vector<std::unique_ptr<label>> m_tab_labels;
		rounded_rect m_tab_underline;
		ellipse m_avatar;
		label m_avatar_letter;
		label m_user_name;

		// Hero
		label m_platform;
		label m_title;
		std::vector<std::unique_ptr<rounded_rect>> m_chips;
		std::vector<std::unique_ptr<label>> m_chip_labels;
		rounded_rect m_play_button;
		image_view m_play_icon;
		label m_play_label;
		ellipse m_settings_button;
		image_view m_settings_icon;
		label m_settings_label;
		std::unique_ptr<image_info> m_play_icon_data;
		std::unique_ptr<image_info> m_settings_icon_data;

		// The games row
		label m_row_title;
		overlay_element m_row_rule;
		std::vector<std::unique_ptr<image_view>> m_tiles;
		std::vector<std::unique_ptr<label>> m_tile_labels;
		rounded_rect m_highlight;
		label m_placeholder;

		// Button prompts
		image_button m_hint_play{120, 26};
		image_button m_hint_settings{120, 26};
		image_button m_hint_l1{30, 26};
		image_button m_hint_r1{120, 26};

		animation_color_interpolate m_fade_animation{};
	};

	// Run on Big Picture Mode's thread (Emulator::BootBigPictureMode); blocks
	// until its shell is torn down, by a game's boot or by leaving it
	void open_ps5_launcher();
}
