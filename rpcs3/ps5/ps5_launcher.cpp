// PS5: the title's game launcher (ps5_launcher.h).
//
// The layout is in RSX's virtual 1280x720 space; the art is the games' own,
// read from their folders (PS3_GAME/ICON0.PNG, PS3_GAME/PIC1.PNG), and the
// game list is Big Picture Mode's (game_enumeration over the games folder and
// dev_hdd0/game). A game boots as Big Picture Mode boots one: its shell stops
// and the game boots in one main-thread call, and the game's stop returns here.

#include "stdafx.h"
#include "ps5_launcher.h"

#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/BigPicture/overlay_big_picture.h"
#include "Emu/RSX/Overlays/BigPicture/overlay_big_picture_game_grid.h"
#include "Emu/RSX/Overlays/HomeMenu/overlay_home_menu_settings.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Utilities/File.h"
#include "Utilities/Thread.h"

#include <algorithm>

LOG_CHANNEL(launcher_log, "Launcher");

namespace rsx::overlays
{
	namespace
	{
		using text_align = overlay_element::text_align;

		// The design's palette
		const color4f c_text{1.f, 1.f, 1.f, 1.f};
		const color4f c_text_dim{1.f, 1.f, 1.f, 0.7f};
		const color4f c_accent{0.35f, 0.85f, 0.95f, 1.f};
		const color4f c_button{0.96f, 0.93f, 0.87f, 1.f};
		const color4f c_button_text{0.08f, 0.08f, 0.1f, 1.f};
		const color4f c_chip{1.f, 1.f, 1.f, 0.14f};
		const color4f c_backdrop{0.03f, 0.04f, 0.09f, 1.f};

		// The games row: ICON0.PNG is 320x176
		constexpr s16 c_row_x = 40;
		constexpr s16 c_row_y = 520;
		constexpr u16 c_tile_w = 226;
		constexpr u16 c_tile_h = 124;
		constexpr u16 c_tile_gap = 14;
		constexpr s32 c_visible_tiles = 5;

		// Tab pages' area, below the top bar
		constexpr s16 c_page_x = 40;
		constexpr s16 c_page_y = 100;
		constexpr u16 c_page_w = 1200;
		constexpr u16 c_page_h = 520;

		void boot_game(std::string path, std::string title_id)
		{
			launcher_log.notice("Booting '%s' (%s)", title_id, path);

			// As Big Picture Mode's own boot_game_from_big_picture_mode: the shell
			// stops and the game boots in one main-thread call, and only then is
			// the session marked as launched from here, so the shell's own stop
			// does not count as the game's
			Emu.CallFromMainThread([path, title_id]()
			{
				Emu.SetContinuousMode(true);
				Emu.GracefulShutdown(false);
				g_big_picture_mode_active = true;

				if (const game_boot_result result = Emu.BootGame(path, title_id); is_error(result))
				{
					launcher_log.error("Booting '%s' failed: %s", path, result);
					g_big_picture_mode_active = false;
					// Back to the launcher rather than an empty screen
					Emu.BootBigPictureMode();
				}
			});
		}

		std::string read_user_name()
		{
			const std::string path = rpcs3::utils::get_hdd0_dir() + "home/" + Emu.GetUsr() + "/localusername";
			if (fs::file file{path})
			{
				std::string name = file.to_string();
				name = name.substr(0, name.find_first_of("\r\n"));
				if (!name.empty())
				{
					return name;
				}
			}
			return "User " + Emu.GetUsr();
		}

		std::unique_ptr<label> make_label(std::string_view text, u16 font_size, const color4f& color)
		{
			auto result = std::make_unique<label>(text);
			result->set_font(font_size);
			result->fore_color = color;
			result->back_color.a = 0.f;
			result->auto_resize();
			return result;
		}

		void style_label(label& target, std::string_view text, u16 font_size, const color4f& color)
		{
			target.set_text(text);
			target.set_font(font_size);
			target.fore_color = color;
			target.back_color.a = 0.f;
			target.auto_resize();
		}

		// "PLAYSTATION 3" with the design's wide letter spacing
		std::string spaced(std::string_view text)
		{
			std::string result;
			for (const char c : text)
			{
				if (!result.empty())
				{
					result += ' ';
				}
				result += c;
			}
			return result;
		}

		std::unique_ptr<image_info> load_image(const std::string& path)
		{
			if (path.empty() || !fs::is_file(path))
			{
				return nullptr;
			}

			auto image = std::make_unique<image_info>(path);
			if (!image->get_data())
			{
				return nullptr;
			}

			// The renderer's texture cache is keyed by address, which a freed image
			// can hand on to the next: upload this one afresh
			image->dirty = true;
			return image;
		}
	}

	ps5_launcher_dialog::ps5_launcher_dialog()
	{
		m_allow_input_on_pause = true;
		m_fade_animation.duration_sec = 0.2f;
		return_code = selection_code::canceled;

		build_static();

		m_library = std::make_shared<big_picture_game_grid>(c_page_x, c_page_y, c_page_w, c_page_h, nullptr, &boot_game);
		m_settings = std::make_shared<home_menu_settings>(c_page_x, c_page_y, c_page_w, c_page_h, false, nullptr);
		m_library->is_current_page = true;
		m_settings->is_current_page = true;

		start_reload();
	}

	ps5_launcher_dialog::~ps5_launcher_dialog()
	{
		m_delete_thread.reset();

		if (m_enumeration_thread)
		{
			*m_enumeration_thread = thread_state::aborting;
			(*m_enumeration_thread)();
			m_enumeration_thread.reset();
		}
	}

	void ps5_launcher_dialog::build_static()
	{
		// Background
		m_background.set_size(virtual_width, virtual_height);
		m_background.back_color = c_backdrop;

		m_wash.set_size(virtual_width, virtual_height);
		m_wash.back_color = color4f(0.f, 0.f, 0.f, 0.15f);

		// The fade from the left edge (where the text sits) and up from the bottom
		// (where the games row sits), as bands of falling opacity
		constexpr int bands = 32;
		for (int i = 0; i < bands; i++)
		{
			auto band = std::make_unique<overlay_element>();
			const u16 band_w = 760 / bands;
			band->set_pos(static_cast<s16>(i * band_w), 0);
			band->set_size(band_w + 1, virtual_height);
			const f32 t = static_cast<f32>(i) / bands;
			band->back_color = color4f(c_backdrop.r, c_backdrop.g, c_backdrop.b, 0.88f * (1.f - t) * (1.f - t));
			m_fades.push_back(std::move(band));
		}
		for (int i = 0; i < bands; i++)
		{
			auto band = std::make_unique<overlay_element>();
			const u16 band_h = 300 / bands;
			band->set_pos(0, static_cast<s16>(virtual_height - (i + 1) * band_h));
			band->set_size(virtual_width, band_h + 1);
			const f32 t = static_cast<f32>(i) / bands;
			band->back_color = color4f(c_backdrop.r, c_backdrop.g, c_backdrop.b, 0.9f * (1.f - t) * (1.f - t));
			m_fades.push_back(std::move(band));
		}

		// Top bar
		style_label(m_logo, "RPCS3", 22, c_text);
		m_logo.set_pos(40, 22);

		for (const char* name : {"Home", "Library", "Settings"})
		{
			m_tab_labels.push_back(make_label(name, 16, c_text_dim));
		}
		m_tab_underline.border_radius = 1;
		m_tab_underline.back_color = c_accent;
		layout_tabs();

		const std::string user = read_user_name();
		style_label(m_user_name, user, 15, c_text);
		m_user_name.set_pos(static_cast<s16>(1240 - m_user_name.w), 28);
		m_avatar.set_size(34, 34);
		m_avatar.set_pos(static_cast<s16>(m_user_name.x - 46), 20);
		m_avatar.back_color = color4f(c_accent.r, c_accent.g, c_accent.b, 0.85f);
		style_label(m_avatar_letter, user.substr(0, 1), 16, c_button_text);
		m_avatar_letter.set_size(34, 34);
		m_avatar_letter.align_text(text_align::center);
		m_avatar_letter.set_pos(m_avatar.x, static_cast<s16>(m_avatar.y + 6));

		// Hero
		style_label(m_platform, spaced("PLAYSTATION 3"), 13, c_text_dim);
		m_platform.set_pos(64, 140);

		m_title.set_font(46);
		m_title.fore_color = c_text;
		m_title.back_color.a = 0.f;
		m_title.set_wrap_text(true);
		m_title.set_pos(60, 168);
		m_title.set_size(600, 170);

		m_play_button.set_size(186, 48);
		m_play_button.border_radius = 24;
		m_play_button.back_color = c_button;

		m_play_icon_data = resource_config::load_icon("home/32/play-button-arrowhead.png");
		m_play_icon.set_size(20, 20);
		m_play_icon.back_color.a = 0.f;
		if (m_play_icon_data)
		{
			m_play_icon_data->dirty = true;
			m_play_icon.set_raw_image(m_play_icon_data.get());
			m_play_icon.fore_color = c_button_text;
		}
		style_label(m_play_label, "Play now", 18, c_button_text);

		m_settings_button.set_size(48, 48);
		m_settings_button.back_color = color4f(1.f, 1.f, 1.f, 0.08f);
		m_settings_button.border_size = 1;
		m_settings_button.border_color = color4f(1.f, 1.f, 1.f, 0.35f);
		m_settings_icon_data = resource_config::load_icon("home/32/settings.png");
		m_settings_icon.set_size(24, 24);
		m_settings_icon.back_color.a = 0.f;
		if (m_settings_icon_data)
		{
			m_settings_icon_data->dirty = true;
			m_settings_icon.set_raw_image(m_settings_icon_data.get());
		}
		style_label(m_settings_label, "Game settings", 16, c_text);

		m_delete_button.border_radius = 24;
		m_delete_button.back_color = color4f(1.f, 1.f, 1.f, 0.08f);
		m_delete_button.border_size = 1;
		m_delete_button.border_color = color4f(1.f, 1.f, 1.f, 0.35f);
		style_label(m_delete_label, "Delete", 16, c_text);
		m_delete_button.set_size(static_cast<u16>(m_delete_label.w + 48), 48);

		// The delete confirmation, centred over a dimmed screen
		m_confirm_dim.set_size(virtual_width, virtual_height);
		m_confirm_dim.back_color = color4f(0.f, 0.f, 0.f, 0.6f);
		m_confirm_panel.set_size(640, 230);
		m_confirm_panel.set_pos((virtual_width - 640) / 2, (virtual_height - 230) / 2);
		m_confirm_panel.border_radius = 18;
		m_confirm_panel.back_color = color4f(0.07f, 0.08f, 0.14f, 0.97f);
		m_confirm_panel.border_size = 1;
		m_confirm_panel.border_color = color4f(1.f, 1.f, 1.f, 0.2f);
		m_confirm_title.set_font(22);
		m_confirm_title.fore_color = c_text;
		m_confirm_title.back_color.a = 0.f;
		m_confirm_title.set_wrap_text(true);
		m_confirm_title.set_pos(static_cast<s16>(m_confirm_panel.x + 32), static_cast<s16>(m_confirm_panel.y + 28));
		m_confirm_title.set_size(576, 60);
		m_confirm_body.set_font(15);
		m_confirm_body.fore_color = c_text_dim;
		m_confirm_body.back_color.a = 0.f;
		m_confirm_body.set_wrap_text(true);
		m_confirm_body.set_pos(static_cast<s16>(m_confirm_panel.x + 32), static_cast<s16>(m_confirm_panel.y + 92));
		m_confirm_body.set_size(576, 80);
		m_confirm_yes.set_image_resource(resource_config::confirm_button_resource());
		m_confirm_yes.set_font(15);
		m_confirm_yes.back_color.a = 0.f;
		m_confirm_yes.set_pos(static_cast<s16>(m_confirm_panel.x + 32), static_cast<s16>(m_confirm_panel.y + 182));
		m_confirm_no.set_image_resource(resource_config::cancel_button_resource());
		m_confirm_no.set_text("Cancel");
		m_confirm_no.set_font(15);
		m_confirm_no.back_color.a = 0.f;
		m_confirm_no.set_pos(static_cast<s16>(m_confirm_panel.x + 200), static_cast<s16>(m_confirm_panel.y + 182));

		// The games row
		style_label(m_row_title, "Your games", 15, c_text);
		m_row_title.set_pos(c_row_x, c_row_y - 36);
		m_row_rule.set_pos(static_cast<s16>(c_row_x + m_row_title.w + 16), static_cast<s16>(c_row_y - 26));
		m_row_rule.set_size(static_cast<u16>(1240 - m_row_rule.x), 1);
		m_row_rule.back_color = color4f(1.f, 1.f, 1.f, 0.22f);

		m_highlight.border_radius = 8;
		m_highlight.border_size = 3;
		m_highlight.border_color = c_accent;
		m_highlight.back_color.a = 0.f;
		m_highlight.set_size(c_tile_w + 8, c_tile_h + 8);

		style_label(m_placeholder, "Looking for games...", 18, c_text_dim);
		m_placeholder.set_pos(c_row_x, c_row_y + 40);

		// Button prompts, bottom right
		const auto hint = [](image_button& button, u8 image, std::string_view text)
		{
			button.set_image_resource(image);
			button.set_text(text);
			button.set_font(15);
			button.back_color.a = 0.f;
		};
		hint(m_hint_play, resource_config::confirm_button_resource(), "Select");
		hint(m_hint_settings, resource_config::standard_image_resource::triangle, "Settings");
		hint(m_hint_delete, resource_config::standard_image_resource::square, "Delete");
		hint(m_hint_l1, resource_config::standard_image_resource::L1, "");
		hint(m_hint_r1, resource_config::standard_image_resource::R1, "Tabs");
		m_hint_r1.set_pos(1150, 676);
		m_hint_l1.set_pos(1116, 676);
		m_hint_delete.set_pos(1000, 676);
		m_hint_settings.set_pos(870, 676);
		m_hint_play.set_pos(750, 676);

		layout_home();
	}

	void ps5_launcher_dialog::layout_tabs()
	{
		s16 x = static_cast<s16>(m_logo.x + m_logo.w + 60);
		for (usz i = 0; i < m_tab_labels.size(); i++)
		{
			label& tab_label = *m_tab_labels[i];
			const bool active = i == static_cast<usz>(m_tab);
			tab_label.fore_color = active ? c_text : c_text_dim;
			tab_label.set_pos(x, 28);
			tab_label.refresh();

			if (active)
			{
				m_tab_underline.set_pos(static_cast<s16>(x - 2), static_cast<s16>(tab_label.y + tab_label.h + 6));
				m_tab_underline.set_size(static_cast<u16>(tab_label.w + 4), 3);
				m_tab_underline.refresh();
			}

			x = static_cast<s16>(x + tab_label.w + 40);
		}
	}

	void ps5_launcher_dialog::layout_home()
	{
		// The title's lines decide where the chips and the buttons go
		const ps5_launcher_game* game = (m_selected >= 0 && static_cast<usz>(m_selected) < m_games.size()) ? &m_games[m_selected] : nullptr;

		m_title.set_text(game ? (game->info.name.empty() ? game->info.serial : game->info.name) : std::string(m_loading ? "" : "No games yet"));
		m_title.set_size(600, 170);
		m_title.auto_resize(false, 600, 170);
		s16 y = static_cast<s16>(m_title.y + m_title.h + 18);

		// Chips: what the game's PARAM.SFO says of it
		m_chips.clear();
		m_chip_labels.clear();
		if (game)
		{
			std::vector<std::string> chips;
			if (!game->info.serial.empty()) chips.push_back(game->info.serial);
			if (game->info.category == "DG") chips.push_back("Disc");
			else if (game->info.category == "HG") chips.push_back("Digital");
			if (!game->info.app_ver.empty() && game->info.app_ver != "Unknown") chips.push_back("Version " + game->info.app_ver);

			s16 x = 64;
			for (const std::string& text : chips)
			{
				auto chip_label = make_label(text, 13, c_text);
				auto chip = std::make_unique<rounded_rect>();
				chip->border_radius = 14;
				chip->back_color = c_chip;
				chip->set_pos(x, y);
				chip->set_size(static_cast<u16>(chip_label->w + 28), 28);
				chip_label->set_pos(static_cast<s16>(x + 14), static_cast<s16>(y + (28 - chip_label->h) / 2));
				x = static_cast<s16>(x + chip->w + 10);
				m_chips.push_back(std::move(chip));
				m_chip_labels.push_back(std::move(chip_label));
			}
			y = static_cast<s16>(y + 28 + 30);
		}
		else
		{
			y = static_cast<s16>(y + 10);
		}

		m_play_button.set_pos(64, y);
		m_play_icon.set_pos(static_cast<s16>(m_play_button.x + 32), static_cast<s16>(y + 14));
		m_play_label.set_pos(static_cast<s16>(m_play_button.x + 66), static_cast<s16>(y + (48 - m_play_label.h) / 2));
		m_settings_button.set_pos(static_cast<s16>(m_play_button.x + m_play_button.w + 24), y);
		m_settings_icon.set_pos(static_cast<s16>(m_settings_button.x + 12), static_cast<s16>(y + 12));
		m_settings_label.set_pos(static_cast<s16>(m_settings_button.x + 62), static_cast<s16>(y + (48 - m_settings_label.h) / 2));
		m_delete_button.set_pos(static_cast<s16>(m_settings_label.x + m_settings_label.w + 36), y);
		m_delete_label.set_pos(static_cast<s16>(m_delete_button.x + 24), static_cast<s16>(y + (48 - m_delete_label.h) / 2));
		m_delete_button.refresh();
		m_delete_label.refresh();

		for (overlay_element* element : std::initializer_list<overlay_element*>{&m_title, &m_play_button, &m_play_icon, &m_play_label, &m_settings_button, &m_settings_icon, &m_settings_label})
		{
			element->refresh();
		}

		// The background: the game's art, or its icon blurred, or none
		if (game && game->background)
		{
			m_background.set_blur_strength(0);
			m_background.set_raw_image(game->background.get());
		}
		else if (game && game->icon)
		{
			m_background.set_blur_strength(80);
			m_background.set_raw_image(game->icon.get());
		}
		else
		{
			m_background.clear_image();
		}
		m_background.refresh();

		// The visible tiles, with the selected one kept in view
		if (m_selected < m_first_visible)
		{
			m_first_visible = m_selected;
		}
		else if (m_selected >= m_first_visible + c_visible_tiles)
		{
			m_first_visible = m_selected - c_visible_tiles + 1;
		}

		m_tiles.clear();
		m_tile_labels.clear();
		for (s32 i = 0; i < c_visible_tiles && static_cast<usz>(m_first_visible + i) < m_games.size(); i++)
		{
			const ps5_launcher_game& entry = m_games[m_first_visible + i];
			const s16 x = static_cast<s16>(c_row_x + i * (c_tile_w + c_tile_gap));
			const bool selected = m_first_visible + i == m_selected;

			auto tile = std::make_unique<image_view>();
			tile->set_pos(x, c_row_y);
			tile->set_size(c_tile_w, c_tile_h);
			tile->back_color = color4f(1.f, 1.f, 1.f, 0.08f);
			if (entry.icon)
			{
				tile->set_raw_image(entry.icon.get());
			}
			else
			{
				tile->set_image_resource(resource_config::standard_image_resource::new_entry);
			}

			auto name = make_label(entry.info.name.empty() ? entry.info.serial : entry.info.name, selected ? 16 : 15, selected ? c_text : c_text_dim);
			name->set_size(c_tile_w, name->h);
			name->set_pos(x, static_cast<s16>(c_row_y + c_tile_h + 12));

			if (selected)
			{
				m_highlight.set_pos(static_cast<s16>(x - 4), static_cast<s16>(c_row_y - 4));
				m_highlight.refresh();
			}

			m_tiles.push_back(std::move(tile));
			m_tile_labels.push_back(std::move(name));
		}

		if (!m_loading)
		{
			style_label(m_placeholder, "No games found. Put each game's folder in /data/homebrew/PPSA99200/rpcs3/games/", 16, c_text_dim);
		}

		layout_focus();
	}

	void ps5_launcher_dialog::layout_focus()
	{
		// The focused button gets the accent's outline; the tile outline shows
		// strongly while the row has the focus, faintly while a button has it
		const auto outline = [](overlay_element& button, bool focused, u8 idle_border)
		{
			button.border_size = focused ? 3 : idle_border;
			button.border_color = focused ? c_accent : color4f(1.f, 1.f, 1.f, 0.35f);
			button.refresh();
		};
		outline(m_play_button, m_focus == focus::play, 0);
		outline(m_settings_button, m_focus == focus::settings, 1);
		outline(m_delete_button, m_focus == focus::remove, 1);

		m_highlight.border_color = m_focus == focus::tiles ? c_accent : color4f(c_accent.r, c_accent.g, c_accent.b, 0.35f);
		m_highlight.pulse_effect_enabled = m_focus == focus::tiles;
		m_highlight.refresh();
	}

	void ps5_launcher_dialog::set_focus(focus next)
	{
		if (next == m_focus)
		{
			return;
		}

		m_focus = next;
		play_sound(sound_effect::cursor);
		layout_focus();
	}

	void ps5_launcher_dialog::ask_delete()
	{
		if (m_selected < 0 || static_cast<usz>(m_selected) >= m_games.size())
		{
			return;
		}

		const big_picture_game_info& info = m_games[m_selected].info;
		m_confirm_title.set_text("Delete " + (info.name.empty() ? info.serial : info.name) + "?");
		m_confirm_body.set_text("This removes the game's files, its compiled code and its place in the library from the console. Saves are kept. It can't be undone.");
		m_confirm_yes.set_text("Delete");
		m_confirm_no.set_visible(true);
		m_confirm_delete = true;
		m_delete_result.clear();
		play_sound(sound_effect::dialog_ok);
	}

	void ps5_launcher_dialog::delete_selected()
	{
		if (m_selected < 0 || static_cast<usz>(m_selected) >= m_games.size() || m_deleting)
		{
			return;
		}

		const big_picture_game_info info = m_games[m_selected].info;

		// Only a folder directly in the games folder or dev_hdd0/game is ever
		// removed: the one the game's path starts in
		std::string root;
		for (std::string base : {rpcs3::utils::get_games_dir(), rpcs3::utils::get_hdd0_game_dir()})
		{
			if (!base.ends_with('/'))
			{
				base += '/';
			}
			if (info.path.starts_with(base) && info.path.size() > base.size())
			{
				const std::string rest = info.path.substr(base.size());
				const std::string first = rest.substr(0, rest.find('/'));
				if (!first.empty() && first != "." && first != ".." && !first.starts_with("$") && !first.starts_with(".") && !first.starts_with("\xef"))
				{
					root = base + first;
				}
			}
		}

		m_confirm_delete = false;
		m_deleting = true;
		m_confirm_title.set_text("Deleting " + (info.name.empty() ? info.serial : info.name) + "...");
		m_confirm_body.set_text("This can take a while for a large game.");

		m_delete_thread = std::make_unique<named_thread<std::function<void()>>>("Launcher Delete", [this, info, root]()
		{
			std::string result;

			if (root.empty())
			{
				launcher_log.error("Not deleting '%s': its path (%s) is not in the games folder or dev_hdd0/game", info.serial, info.path);
				result = "This game isn't in the app's games folder, so its files were left alone. It was removed from the library.";
			}
			else if (fs::is_dir(root) && !fs::remove_all(root))
			{
				launcher_log.error("Deleting %s failed: %s", root, fs::g_tls_error);
				result = fmt::format("Some of the files couldn't be deleted (%s). Delete the folder over FTP instead:\n%s", fs::g_tls_error, root.substr(root.find("/rpcs3/") == umax ? 0 : root.find("/rpcs3/") + 1));
			}
			else
			{
				launcher_log.notice("Deleted %s", root);
			}

			// Its compiled code and its entry in games.yml
			if (!info.serial.empty())
			{
				const std::string cache = rpcs3::utils::get_cache_dir_by_serial(info.serial);
				if (!cache.empty() && fs::is_dir(cache))
				{
					fs::remove_all(cache);
				}
				Emu.RemoveGames({info.serial});
			}

			{
				std::lock_guard lock(m_mutex);
				m_delete_result = result;
				if (!result.empty())
				{
					m_confirm_title.set_text("Couldn't delete everything");
					m_confirm_body.set_text(result);
					m_confirm_yes.set_text("OK");
					m_confirm_no.set_visible(false);
				}
			}

			m_deleting = false;
			m_reload_requested = true;
		});
	}

	void ps5_launcher_dialog::start_reload()
	{
		m_loading = true;

		m_enumeration.initialize_paths();
		m_enumeration.set_localization(g_cfg.sys.language, "Unknown", [](const std::string&) { return std::string(); });
		m_enumeration.set_show_custom_icons(true);
		m_enumeration.set_prefer_game_data_icons(true);
		m_enumeration.set_play_hover_movies(false);
		m_enumeration.set_play_hover_music(false);
		m_enumeration.set_canceled_callback([]() { return thread_ctrl::state() == thread_state::aborting; });

		m_enumeration_thread = std::make_unique<named_thread<std::function<void()>>>("Launcher Reload", [this]()
		{
			m_enumeration.parse_directories();
			m_enumeration.add_vfs_entry();
			m_enumeration.remove_duplicates();

			for (const auto& entry : m_enumeration.path_entries())
			{
				m_enumeration.parse_entry(entry);
			}
			m_enumeration.apply_patches();

			std::vector<ps5_launcher_game> games;
			for (big_picture_game_info& info : m_enumeration.take_games())
			{
				if (!info.bootable)
				{
					continue;
				}

				ps5_launcher_game game;
				game.icon = info.load_icon();
				if (game.icon)
				{
					game.icon->dirty = true;
				}

				// PIC1.PNG sits beside ICON0.PNG in the game's PS3_GAME folder
				if (!info.icon_in_archive && !info.icon_path.empty())
				{
					game.background = load_image(fs::get_parent_dir(info.icon_path) + "/PIC1.PNG");
				}

				game.info = std::move(info);
				games.push_back(std::move(game));
			}
			m_enumeration.clear(true);

			std::sort(games.begin(), games.end(), [](const ps5_launcher_game& a, const ps5_launcher_game& b)
			{
				return a.info.name < b.info.name;
			});

			launcher_log.notice("%u games found", games.size());

			std::lock_guard lock(m_mutex);
			if (thread_ctrl::state() == thread_state::aborting)
			{
				return;
			}
			m_games = std::move(games);
			m_selected = 0;
			m_first_visible = 0;
			m_loading = false;
			layout_home();
		});
	}

	void ps5_launcher_dialog::select_game(s32 index)
	{
		if (m_games.empty())
		{
			return;
		}

		index = std::clamp(index, 0, static_cast<s32>(m_games.size()) - 1);
		if (index == m_selected)
		{
			return;
		}

		m_selected = index;
		play_sound(sound_effect::cursor);
		layout_home();
	}

	void ps5_launcher_dialog::set_tab(tab next)
	{
		if (next == m_tab)
		{
			return;
		}

		m_tab = next;
		play_sound(sound_effect::cursor);
		layout_tabs();

		if (m_tab == tab::library)
		{
			m_library->on_activate();
		}
		else if (m_tab == tab::settings)
		{
			m_settings->on_activate();
		}
	}

	void ps5_launcher_dialog::boot_selected()
	{
		if (m_selected < 0 || static_cast<usz>(m_selected) >= m_games.size())
		{
			return;
		}

		const big_picture_game_info& info = m_games[m_selected].info;
		play_sound(sound_effect::accept);
		boot_game(info.path, info.serial);
	}

	void ps5_launcher_dialog::update(u64 timestamp_us)
	{
		if (m_reload_requested.exchange(false))
		{
			m_enumeration_thread.reset();
			start_reload();
		}

		if (m_fade_animation.active)
		{
			m_fade_animation.update(timestamp_us);
		}

		if (m_tab == tab::library)
		{
			m_library->update(timestamp_us);
		}
		else if (m_tab == tab::settings)
		{
			m_settings->update(timestamp_us);
		}
	}

	void ps5_launcher_dialog::on_button_pressed(pad_button button_press, bool is_auto_repeat)
	{
		if (m_fade_animation.active)
		{
			return;
		}

		std::lock_guard lock(m_mutex);

		// The delete confirmation, or its result, takes every button
		if (m_deleting)
		{
			return;
		}
		if (m_confirm_delete || !m_delete_result.empty())
		{
			if (button_press == pad_button::cross && m_confirm_delete)
			{
				delete_selected();
			}
			else if (button_press == pad_button::cross || button_press == pad_button::circle)
			{
				m_confirm_delete = false;
				m_delete_result.clear();
				play_sound(sound_effect::cancel);
			}
			return;
		}

		// Tabs change from anywhere at the top level
		if (button_press == pad_button::L1 || button_press == pad_button::R1)
		{
			const s32 step = button_press == pad_button::L1 ? -1 : 1;
			set_tab(static_cast<tab>(std::clamp(static_cast<s32>(m_tab) + step, 0, 2)));
			return;
		}

		if (m_tab != tab::home)
		{
			home_menu_page& page = m_tab == tab::library ? *m_library : *m_settings;
			// Circle at a page's top goes back to Home
			if (page.handle_button_press(button_press, is_auto_repeat, m_auto_repeat_ms_interval) == page_navigation::exit)
			{
				set_tab(tab::home);
			}
			return;
		}

		if (m_games.empty())
		{
			return;
		}

		const bool up = button_press == pad_button::dpad_up || button_press == pad_button::ls_up;
		const bool down = button_press == pad_button::dpad_down || button_press == pad_button::ls_down;
		const bool left = button_press == pad_button::dpad_left || button_press == pad_button::ls_left;
		const bool right = button_press == pad_button::dpad_right || button_press == pad_button::ls_right;

		if (button_press == pad_button::triangle)
		{
			set_tab(tab::settings);
			return;
		}
		if (button_press == pad_button::square)
		{
			ask_delete();
			return;
		}

		if (m_focus == focus::tiles)
		{
			if (left) select_game(m_selected - 1);
			else if (right) select_game(m_selected + 1);
			else if (up) set_focus(focus::play);
			else if (button_press == pad_button::cross) boot_selected();
			return;
		}

		// The buttons' row
		constexpr focus order[] = {focus::play, focus::settings, focus::remove};
		const s32 at = static_cast<s32>(std::find(std::begin(order), std::end(order), m_focus) - std::begin(order));

		if (left && at > 0) set_focus(order[at - 1]);
		else if (right && at < 2) set_focus(order[at + 1]);
		else if (down || button_press == pad_button::circle) set_focus(focus::tiles);
		else if (button_press == pad_button::cross)
		{
			switch (m_focus)
			{
			case focus::play: boot_selected(); break;
			case focus::settings: set_tab(tab::settings); break;
			case focus::remove: ask_delete(); break;
			default: break;
			}
		}
	}

	compiled_resource ps5_launcher_dialog::get_compiled()
	{
		if (!visible)
		{
			return {};
		}

		std::lock_guard lock(m_mutex);

		compiled_resource result;
		result.add(m_background.get_compiled());
		result.add(m_wash.get_compiled());
		for (const auto& fade : m_fades)
		{
			result.add(fade->get_compiled());
		}

		if (m_tab == tab::home)
		{
			result.add(m_platform.get_compiled());
			result.add(m_title.get_compiled());
			for (usz i = 0; i < m_chips.size(); i++)
			{
				result.add(m_chips[i]->get_compiled());
				result.add(m_chip_labels[i]->get_compiled());
			}

			if (!m_games.empty())
			{
				result.add(m_play_button.get_compiled());
				result.add(m_play_icon.get_compiled());
				result.add(m_play_label.get_compiled());
				result.add(m_settings_button.get_compiled());
				result.add(m_settings_icon.get_compiled());
				result.add(m_settings_label.get_compiled());
				result.add(m_delete_button.get_compiled());
				result.add(m_delete_label.get_compiled());
			}

			result.add(m_row_title.get_compiled());
			result.add(m_row_rule.get_compiled());

			if (m_games.empty())
			{
				result.add(m_placeholder.get_compiled());
			}
			else
			{
				for (usz i = 0; i < m_tiles.size(); i++)
				{
					result.add(m_tiles[i]->get_compiled());
					result.add(m_tile_labels[i]->get_compiled());
				}
				result.add(m_highlight.get_compiled());

				result.add(m_hint_play.get_compiled());
				result.add(m_hint_settings.get_compiled());
				result.add(m_hint_delete.get_compiled());
			}
		}
		else
		{
			// A darker wash under the stock pages, for their text
			overlay_element dim;
			dim.set_size(virtual_width, virtual_height);
			dim.back_color = color4f(0.f, 0.f, 0.f, 0.7f);
			result.add(dim.get_compiled());
			result.add((m_tab == tab::library ? m_library : m_settings)->get_compiled());
		}

		// The top bar over everything
		result.add(m_logo.get_compiled());
		for (const auto& tab_label : m_tab_labels)
		{
			result.add(tab_label->get_compiled());
		}
		result.add(m_tab_underline.get_compiled());
		result.add(m_avatar.get_compiled());
		result.add(m_avatar_letter.get_compiled());
		result.add(m_user_name.get_compiled());
		result.add(m_hint_l1.get_compiled());
		result.add(m_hint_r1.get_compiled());

		if (m_confirm_delete || m_deleting || !m_delete_result.empty())
		{
			result.add(m_confirm_dim.get_compiled());
			result.add(m_confirm_panel.get_compiled());
			result.add(m_confirm_title.get_compiled());
			result.add(m_confirm_body.get_compiled());
			if (!m_deleting)
			{
				result.add(m_confirm_yes.get_compiled());
				if (m_confirm_delete)
				{
					result.add(m_confirm_no.get_compiled());
				}
			}
		}

		m_fade_animation.apply(result);
		return result;
	}

	void ps5_launcher_dialog::show()
	{
		m_fade_animation.current = color4f(0.f);
		m_fade_animation.end = color4f(1.f);
		m_fade_animation.active = true;

		visible = true;

		auto& overlayman = g_fxo->get<display_manager>();
		overlayman.attach_thread_input(uid, "PS5 Launcher");
	}

	void open_ps5_launcher()
	{
		auto& overlayman = g_fxo->get<display_manager>();
		const auto dialog = overlayman.create<ps5_launcher_dialog>();
		dialog->show();

		// Both ways out (a game's boot, or leaving) tear the shell down from the
		// main thread, and this dialog with it: wait for that, as Big Picture
		// Mode's own dialog does
		while (!Emu.IsStopped())
		{
			thread_ctrl::wait_for(50'000);
		}
	}
}
