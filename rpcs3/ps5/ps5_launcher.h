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

		// What the D-pad has on Home: the games row, or one of the buttons
		enum class focus : u8
		{
			tiles,
			play,
			settings,
			remove
		};

		void start_reload();
		void select_game(s32 index);
		void set_tab(tab next);
		void set_focus(focus next);
		void boot_selected();
		void ask_delete();
		void delete_selected();
		void layout_focus();

		void build_static();
		void layout_tabs();
		void layout_home();

		std::mutex m_mutex;
		std::unique_ptr<named_thread<std::function<void()>>> m_enumeration_thread;
		game_enumeration<big_picture_game_info> m_enumeration;
		std::vector<ps5_launcher_game> m_games;
		atomic_t<bool> m_loading = true;

		tab m_tab = tab::home;
		focus m_focus = focus::tiles;
		s32 m_selected = 0;

		// Deleting a game: asked, then done on a thread of its own, then the
		// list read again (update)
		bool m_confirm_delete = false;
		atomic_t<bool> m_deleting = false;
		atomic_t<bool> m_reload_requested = false;
		std::unique_ptr<named_thread<std::function<void()>>> m_delete_thread;
		std::string m_delete_result; // shown until dismissed, when the deletion failed
		s32 m_first_visible = 0;

		// The page shown on the Library and Settings tabs
		std::shared_ptr<home_menu_page> m_library;
		std::shared_ptr<home_menu_page> m_settings;

		// Background: the selected game's art over the last one's while it fades
		// in, a dark wash, and smooth fades from the left, the top and the bottom
		overlay_element m_backdrop;
		image_view m_background;
		image_view m_background_prev;
		const image_info_base* m_background_image = nullptr;
		u8 m_background_blur = 0;
		u64 m_background_fade_start = 0;
		bool m_background_fading = false;
		overlay_element m_wash;
		image_view m_fade_left;
		image_view m_fade_top;
		image_view m_fade_bottom;
		std::vector<u8> m_fade_left_pixels;
		std::vector<u8> m_fade_top_pixels;
		std::vector<u8> m_fade_bottom_pixels;
		std::unique_ptr<memory_image_info> m_fade_left_image;
		std::unique_ptr<memory_image_info> m_fade_top_image;
		std::unique_ptr<memory_image_info> m_fade_bottom_image;

		// Top bar
		image_view m_logo;
		std::unique_ptr<image_info> m_logo_data;
		label m_logo_text; // without the logo's image
		overlay_element m_bar_divider;
		std::vector<std::unique_ptr<label>> m_tab_labels;
		rounded_rect m_tab_underline;
		ellipse m_avatar;
		label m_avatar_letter;
		label m_user_name;

		// Hero
		label m_welcome;
		label m_title;
		std::vector<std::unique_ptr<rounded_rect>> m_chips;
		std::vector<std::unique_ptr<label>> m_chip_labels;
		rounded_rect m_play_button;
		image_view m_play_icon;
		label m_play_label;
		ellipse m_settings_button;
		image_view m_settings_icon;
		label m_settings_label;
		ellipse m_delete_button;
		image_view m_delete_icon;
		label m_delete_label;
		std::unique_ptr<image_info> m_play_icon_data;
		std::unique_ptr<image_info> m_settings_icon_data;
		std::unique_ptr<image_info> m_delete_icon_data;

		// The games row
		label m_row_title;
		overlay_element m_row_rule;
		std::vector<std::unique_ptr<image_view>> m_tiles;
		std::vector<std::unique_ptr<label>> m_tile_labels;
		rounded_rect m_highlight;
		label m_placeholder;

		// A button's glyph and what it does
		struct hint
		{
			image_view icon;
			label text;
		};

		// The delete confirmation
		overlay_element m_confirm_dim;
		rounded_rect m_confirm_panel;
		label m_confirm_title;
		label m_confirm_body;
		hint m_confirm_yes;
		hint m_confirm_no;

		// Button prompts, bottom right
		std::vector<std::unique_ptr<hint>> m_hints;
		void layout_hints();
		void layout_confirm();

		animation_color_interpolate m_fade_animation{};

		// The opening: once per run, the logo over a loading line, gliding into
		// the top bar while the art fades in; then, on every opening, the hero
		// and the games row rise into place once the list is read
		bool m_play_intro = false;
		u64 m_now_us = 0;
		u64 m_intro_start_us = 0;
		u64 m_content_start_us = 0;
		f32 intro_seconds() const;
		f32 content_seconds() const;
		bool intro_running() const;
		void skip_intro();
	};

	// Run on Big Picture Mode's thread (Emulator::BootBigPictureMode); blocks
	// until its shell is torn down, by a game's boot or by leaving it
	void open_ps5_launcher();
}
