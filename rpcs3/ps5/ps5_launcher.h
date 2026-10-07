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

		// The Library's cover: the player's own (config/covers/<serial>.png or
		// .jpg), else one drawn from the game's art
		std::unique_ptr<image_info> cover_file;
		std::vector<u8> cover_pixels;
		std::unique_ptr<memory_image_info> cover_drawn;

		const image_info_base* cover() const
		{
			return cover_file ? static_cast<const image_info_base*>(cover_file.get()) : cover_drawn.get();
		}

		// The part of the player's cover that is not transparent (u0, u1, v0,
		// v1): a cover with clear margins still fills the case's front
		f32 cover_crop[4]{0.f, 1.f, 0.f, 1.f};

		// The case's back: the player's own (<serial>-back.png or .jpg), else
		// one drawn from the game's icon and art
		std::unique_ptr<image_info> back_file;
		std::vector<u8> back_pixels;
		std::unique_ptr<memory_image_info> back_drawn;
		f32 back_crop[4]{0.f, 1.f, 0.f, 1.f};

		const image_info_base* back() const
		{
			return back_file ? static_cast<const image_info_base*>(back_file.get()) : back_drawn.get();
		}

		// The case's spine: the player's own (<serial>-spine.png or .jpg), else
		// one drawn with the game's name down it
		std::unique_ptr<image_info> spine_file;
		std::vector<u8> spine_pixels;
		std::unique_ptr<memory_image_info> spine_drawn;
		f32 spine_crop[4]{0.f, 1.f, 0.f, 1.f};

		const image_info_base* spine() const
		{
			return spine_file ? static_cast<const image_info_base*>(spine_file.get()) : spine_drawn.get();
		}
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

		// Game settings: a page of the selected game's own settings, kept in its
		// custom config (config/custom_configs/config_<serial>.yml), which the
		// emulator lays over the global config when the game boots
		struct game_setting
		{
			std::string section;
			std::string key;
			std::string label;
			std::string help;
			std::vector<std::string> options;
			std::string global;
			std::string value;
			bool heading = false;
		};

		bool m_gs_open = false;
		u64 m_gs_open_us = 0;
		std::string m_gs_serial;
		std::string m_gs_name;
		std::vector<game_setting> m_gs_rows;
		s32 m_gs_selected = 0;
		s32 m_gs_scroll = 0;
		std::vector<std::unique_ptr<overlay_element>> m_gs_items;

		void open_game_settings();
		void close_game_settings();
		void layout_game_settings();
		void handle_game_settings(pad_button button_press);

		// The Library: the covers in a row that slides to the selection
		f32 m_flow_pos = 0.f;
		u64 m_last_update_us = 0;
		image_view m_library_art;
		// The selected case's turn (right stick, and a sway of its own), and
		// the game's own menu, which its case zooms into
		f32 m_cover_yaw = 0.f;
		f32 m_cover_pitch = 0.f;
		f32 m_sway_time = 0.f;
		bool m_detail = false;
		bool m_flipped = false;    // the selected case shows its back
		f32 m_flip_angle = 0.f;    // eases to pi when flipped
		std::vector<u8> m_sheen_pixels;
		std::unique_ptr<memory_image_info> m_sheen_image;
		std::unique_ptr<image_info> m_flip_icon_data;
		std::unique_ptr<image_info> m_back_icon_data;
		f32 m_detail_t = 0.f;
		s32 m_detail_option = 0;
		std::vector<u8> m_glow_pixels;
		std::unique_ptr<memory_image_info> m_glow_image;
		void handle_library(pad_button button_press);
		void compile_library(compiled_resource& result);

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
