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
#include "Emu/RSX/Overlays/HomeMenu/overlay_home_menu_settings.h"
#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Utilities/Config.h"
#include "Utilities/File.h"
#include "Utilities/StrUtil.h"
#include "Utilities/Thread.h"

#include <algorithm>
#include <cctype>
#include <cmath>

LOG_CHANNEL(launcher_log, "Launcher");

extern std::string g_cfg_defaults; // Emu/System.cpp: the config's defaults, as text

namespace rsx::overlays
{
	namespace
	{
		// The design's palette
		const color4f c_text{1.f, 1.f, 1.f, 1.f};
		const color4f c_text_dim{1.f, 1.f, 1.f, 0.68f};
		const color4f c_accent{0.40f, 0.87f, 0.95f, 1.f};
		const color4f c_button{0.96f, 0.93f, 0.87f, 1.f};
		const color4f c_button_text{0.06f, 0.06f, 0.08f, 1.f};
		const color4f c_glass{0.02f, 0.03f, 0.07f, 0.42f};
		const color4f c_glass_border{1.f, 1.f, 1.f, 0.22f};
		const color4f c_backdrop{0.02f, 0.03f, 0.08f, 1.f};

		// The title's fonts (/app0/assets/fonts, the frontend's font folder);
		// characters they lack come from the PS3's own font
		constexpr std::string_view f_regular = "Inter-Regular";
		constexpr std::string_view f_medium = "Inter-Medium";
		constexpr std::string_view f_semibold = "Inter-SemiBold";
		constexpr std::string_view f_bold = "Inter-Bold";

		// The overlays' 1280x720 space: margins, the top bar's middle line, the hero's left edge
		constexpr s16 c_margin = 40;
		constexpr s16 c_right = 1240;
		constexpr s16 c_bar_y = 36;
		constexpr s16 c_hero_x = 64;

		// The games row: five tiles across the margins. ICON0.PNG is 320x176
		constexpr s16 c_row_y = 500;
		constexpr u16 c_tile_w = 232;
		constexpr u16 c_tile_h = 128;
		constexpr u16 c_tile_gap = 10;
		constexpr u16 c_tile_radius = 8;
		constexpr s32 c_visible_tiles = 5;

		// The hints' line, bottom right
		constexpr s16 c_hints_y = 690;

		// Tab pages' area, below the top bar
		constexpr s16 c_page_x = 40;
		constexpr s16 c_page_y = 100;
		constexpr u16 c_page_w = 1200;
		constexpr u16 c_page_h = 520;

		// A new game's art fades in over the last one's
		constexpr u64 c_background_fade_us = 220'000;

		// An image with rounded corners: the overlays' rounded-box SDF, which the
		// shader applies to sampled images as to flat colour
		struct rounded_image : public image_view
		{
			u16 border_radius = 0;

			compiled_resource& get_compiled() override
			{
				if (is_compiled())
				{
					return compiled_resources;
				}

				image_view::get_compiled();
				if (!compiled_resources.draw_commands.empty())
				{
					auto& config = compiled_resources.draw_commands.front().config;
					configure_sdf(config, sdf_function::rounded_box);
					config.sdf_config.br = std::min({static_cast<f32>(border_radius), config.sdf_config.hx, config.sdf_config.hy});
				}
				return compiled_resources;
			}
		};

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
				name = name.substr(0, name.find_first_of(std::string_view("\r\n\0", 3)));
				if (!name.empty())
				{
					return name;
				}
			}
			return "User " + Emu.GetUsr();
		}

		void style_label(label& target, std::string_view text, u16 font_size, std::string_view font_name, const color4f& color)
		{
			target.set_text(text);
			target.set_font(font_size, font_name);
			target.fore_color = color;
			target.back_color.a = 0.f;
			target.set_padding(0);
			target.auto_resize();
		}

		std::unique_ptr<label> make_label(std::string_view text, u16 font_size, std::string_view font_name, const color4f& color)
		{
			auto result = std::make_unique<label>();
			style_label(*result, text, font_size, font_name, color);
			return result;
		}

		// The extents of what a text draws, from its baseline at 0 (y up is negative)
		struct ink
		{
			f32 left = 0.f, right = 0.f, top = 0.f, bottom = 0.f;
		};

		ink measure_ink(font* renderer, std::u32string_view text)
		{
			const std::u32string copy(text);
			const std::vector<vertex> verts = renderer->render_text(copy.c_str());
			ink result{};
			bool first = true;
			for (const vertex& v : verts)
			{
				const f32 x = v.values[0];
				const f32 y = v.values[1];
				result.left = first ? x : std::min(result.left, x);
				result.right = first ? x : std::max(result.right, x);
				result.top = first ? y : std::min(result.top, y);
				result.bottom = first ? y : std::max(result.bottom, y);
				first = false;
			}
			return result;
		}

		// A label's top for its capitals to sit centred on mid_y: the font's 'H'
		// decides, so labels in one line share a baseline whatever their letters
		s16 cap_centred_y(const label& target, f32 mid_y)
		{
			font* renderer = target.get_font();
			const ink cap = measure_ink(renderer, U"H");
			// The label draws its baseline at its top plus the font's pixel size
			return static_cast<s16>(std::lround(mid_y - renderer->get_size_px() - (cap.top + cap.bottom) / 2.f));
		}

		void place(label& target, s16 x, f32 mid_y)
		{
			target.set_pos(x, cap_centred_y(target, mid_y));
		}

		// Shortened with an ellipsis to fit max_w
		void fit_text(label& target, std::string_view text, u16 max_w)
		{
			target.set_text(text);
			target.auto_resize();
			if (target.w <= max_w)
			{
				return;
			}

			std::u32string chars = utf8_to_u32string(text);
			while (!chars.empty())
			{
				chars.pop_back();
				while (!chars.empty() && chars.back() == U' ')
				{
					chars.pop_back();
				}
				target.set_unicode_text(chars + U"…");
				target.auto_resize();
				if (target.w <= max_w)
				{
					return;
				}
			}
		}

		// "WELCOME BACK" with the design's wide letter spacing
		std::string spaced(std::string_view text)
		{
			std::string result;
			for (const char c : text)
			{
				if (!result.empty())
				{
					result += c == ' ' ? "  " : " ";
				}
				if (c != ' ')
				{
					result += c;
				}
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

		// A white ramp, its alpha falling as an eased curve from `from` to 0 along
		// its length; tinted by the view's colour and stretched, it is a fade
		// without the bands of stacked rectangles
		std::unique_ptr<memory_image_info> make_ramp(std::vector<u8>& pixels, u16 length, bool horizontal, f32 from)
		{
			pixels.resize(usz{length} * 4);
			for (u16 i = 0; i < length; i++)
			{
				const f32 t = static_cast<f32>(i) / (length - 1);
				const f32 eased = (1.f - t) * (1.f - t) * (3.f - 2.f * (1.f - t)) * 0.5f + (1.f - t) * (1.f - t) * 0.5f;
				pixels[i * 4 + 0] = 255;
				pixels[i * 4 + 1] = 255;
				pixels[i * 4 + 2] = 255;
				pixels[i * 4 + 3] = static_cast<u8>(std::lround(255.f * from * eased));
			}
			auto image = std::make_unique<memory_image_info>(horizontal ? length : u16{1}, horizontal ? u16{1} : length, u8{4}, pixels.data());
			image->dirty = true;
			return image;
		}

		// Covers: a PS3 case's shape (about 1 wide to 1.16 high)
		constexpr u16 c_cover_w = 360;
		constexpr u16 c_cover_h = 416;

		// A pixel of an RGBA image, bilinear, at (u, v) in 0..1
		void sample(const image_info_base& image, f32 u, f32 v, f32 out[4])
		{
			const u8* data = image.get_data();
			const f32 x = std::clamp(u * image.w - 0.5f, 0.f, image.w - 1.f);
			const f32 y = std::clamp(v * image.h - 0.5f, 0.f, image.h - 1.f);
			const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
			const int x1 = std::min(x0 + 1, image.w - 1), y1 = std::min(y0 + 1, image.h - 1);
			const f32 fx = x - x0, fy = y - y0;
			for (int c = 0; c < 4; c++)
			{
				const f32 a = data[(y0 * image.w + x0) * 4 + c] * (1 - fx) + data[(y0 * image.w + x1) * 4 + c] * fx;
				const f32 b = data[(y1 * image.w + x0) * 4 + c] * (1 - fx) + data[(y1 * image.w + x1) * 4 + c] * fx;
				out[c] = a * (1 - fy) + b * fy;
			}
		}

		// A cover drawn from the game's own art: its PIC1 (or its icon) filling
		// the case, darkened toward the foot, its icon across the middle, and a
		// band at the top in the app's colours
		void draw_cover(ps5_launcher_game& game)
		{
			const image_info_base* fill = game.background && game.background->get_data() ? game.background.get() : (game.icon && game.icon->get_data() ? game.icon.get() : nullptr);
			const image_info_base* icon = game.icon && game.icon->get_data() ? game.icon.get() : nullptr;

			std::vector<u8>& px = game.cover_pixels;
			px.resize(usz{c_cover_w} * c_cover_h * 4);

			constexpr u16 band = 34;
			f32 rgba[4];

			// The fill's art boiled down to 6x7 averaged pixels: stretched over
			// the case, a soft wash of its colours with nothing busy in it
			constexpr u16 soft_w = 6, soft_h = 7;
			std::vector<u8> soft_pixels(usz{soft_w} * soft_h * 4, 0);
			if (fill)
			{
				const u8* data = fill->get_data();
				for (u16 sy = 0; sy < soft_h; sy++)
				{
					for (u16 sx = 0; sx < soft_w; sx++)
					{
						const int x0 = sx * fill->w / soft_w, x1 = std::max(x0 + 1, (sx + 1) * fill->w / soft_w);
						const int y0 = sy * fill->h / soft_h, y1 = std::max(y0 + 1, (sy + 1) * fill->h / soft_h);
						u64 sum[4]{};
						u64 n = 0;
						for (int yy = y0; yy < y1; yy += 2)
						{
							for (int xx = x0; xx < x1; xx += 2)
							{
								for (int c = 0; c < 4; c++) sum[c] += data[(yy * fill->w + xx) * 4 + c];
								n++;
							}
						}
						for (int c = 0; c < 4; c++) soft_pixels[(sy * soft_w + sx) * 4 + c] = static_cast<u8>(sum[c] / std::max<u64>(n, 1));
					}
				}
			}
			memory_image_info soft(soft_w, soft_h, u8{4}, soft_pixels.data());
			for (u16 y = 0; y < c_cover_h; y++)
			{
				for (u16 x = 0; x < c_cover_w; x++)
				{
					f32 r = 10, g = 14, b = 30;
					if (fill)
					{
						// The art's colours only: its tiny version, stretched
						const f32 u = (x + 0.5f) / c_cover_w;
						const f32 v = (y + 0.5f) / c_cover_h;
						sample(soft, u, v, rgba);
						const f32 shade = 0.7f - 0.35f * (static_cast<f32>(y) / c_cover_h);
						r = rgba[0] * shade, g = rgba[1] * shade, b = rgba[2] * shade;
					}
					if (y < band)
					{
						r = 6, g = 8, b = 18;
					}
					else if (y < band + 3)
					{
						r = 102, g = 222, b = 242;
					}
					u8* out = &px[(usz{y} * c_cover_w + x) * 4];
					out[0] = static_cast<u8>(std::clamp(r, 0.f, 255.f));
					out[1] = static_cast<u8>(std::clamp(g, 0.f, 255.f));
					out[2] = static_cast<u8>(std::clamp(b, 0.f, 255.f));
					out[3] = 255;
				}
			}

			if (icon)
			{
				// The icon at 86% of the width, a little below the middle
				const u16 iw = static_cast<u16>(c_cover_w * 0.86f);
				const u16 ih = static_cast<u16>(iw * static_cast<f32>(icon->h) / icon->w);
				const u16 ix = static_cast<u16>((c_cover_w - iw) / 2);
				const u16 iy = static_cast<u16>(std::min<int>(c_cover_h - ih - 16, static_cast<int>(c_cover_h * 0.56f - ih / 2.f)));
				for (u16 y = 0; y < ih; y++)
				{
					for (u16 x = 0; x < iw; x++)
					{
						sample(*icon, (x + 0.5f) / iw, (y + 0.5f) / ih, rgba);
						u8* out = &px[(static_cast<usz>(iy + y) * c_cover_w + ix + x) * 4];
						const f32 a = rgba[3] / 255.f;
						for (int c = 0; c < 3; c++)
						{
							out[c] = static_cast<u8>(std::clamp(rgba[c] * a + out[c] * (1.f - a), 0.f, 255.f));
						}
					}
				}
			}

			game.cover_drawn = std::make_unique<memory_image_info>(c_cover_w, c_cover_h, u8{4}, px.data());
			game.cover_drawn->dirty = true;
		}

		// The player's own cover if there is one, else one drawn
		void load_cover(ps5_launcher_game& game)
		{
			if (!game.info.serial.empty())
			{
				const std::string base = fs::get_config_dir(true) + "covers/" + game.info.serial;
				for (const char* extension : {".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG"})
				{
					if (auto image = load_image(base + extension))
					{
						game.cover_file = std::move(image);
						return;
					}
				}
			}
			draw_cover(game);
		}

		// The region a serial's third letter names
		std::string region_of(std::string_view serial)
		{
			if (serial.size() < 3) return {};
			switch (serial[2])
			{
			case 'U': return "USA";
			case 'E': return "Europe";
			case 'J': case 'P': return "Japan";
			case 'A': case 'H': return "Asia";
			case 'K': return "Korea";
			default: return {};
			}
		}

		// The opening's curves: how far a step of it is, at `now` seconds, for a
		// step from `start` lasting `length`
		f32 progress(f32 now, f32 start, f32 length)
		{
			return std::clamp((now - start) / length, 0.f, 1.f);
		}

		f32 ease_out(f32 t)
		{
			return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
		}

		f32 ease_in_out(f32 t)
		{
			return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
		}

		// Part of the screen drawn faded and moved, as the opening has it
		void add_animated(compiled_resource& out, const compiled_resource& part, f32 alpha, f32 dx = 0.f, f32 dy = 0.f)
		{
			if (alpha <= 0.f)
			{
				return;
			}
			if (alpha >= 1.f && dx == 0.f && dy == 0.f)
			{
				out.add(part);
				return;
			}

			compiled_resource faded = part;
			for (auto& cmd : faded.draw_commands)
			{
				cmd.config.color.a *= alpha;
				cmd.config.sdf_config.border_color.a *= alpha;
			}
			out.add(faded, dx, dy);
		}

		// Its timeline, in seconds from the launcher's first frame
		constexpr f32 c_intro_logo_in = 0.1f;      // the logo fades in, centred
		constexpr f32 c_intro_line = 0.25f;        // the loading line draws under it
		constexpr f32 c_intro_move = 1.05f;        // the logo glides into the top bar
		constexpr f32 c_intro_move_length = 0.6f;
		constexpr f32 c_intro_reveal = 1.1f;       // the splash's backdrop fades away
		constexpr f32 c_intro_bar = 1.4f;          // the tabs and the user come down
		constexpr f32 c_intro_content = 1.45f;     // the hero and the row may start
		constexpr f32 c_intro_end = 2.2f;

		// Each opening's own, from when the list is read (and the splash allows)
		constexpr f32 c_content_end = 1.3f;

		// Played once per run of the app
		bool s_intro_played = false;
	}

	ps5_launcher_dialog::ps5_launcher_dialog()
	{
		m_allow_input_on_pause = true;
		m_fade_animation.duration_sec = 0.2f;
		m_play_intro = !std::exchange(s_intro_played, true);
		return_code = selection_code::canceled;

		build_static();
		fs::create_path(fs::get_config_dir(true) + "covers/");

		m_settings = std::make_shared<home_menu_settings>(c_page_x, c_page_y, c_page_w, c_page_h, false, nullptr);
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
		// Background: the art, a light wash, and the fades that seat the text on it
		m_backdrop.set_size(virtual_width, virtual_height);
		m_backdrop.back_color = c_backdrop;
		for (image_view* view : {&m_background, &m_background_prev})
		{
			view->set_size(virtual_width, virtual_height);
			view->back_color.a = 0.f;
		}

		m_wash.set_size(virtual_width, virtual_height);
		m_wash.back_color = color4f(0.f, 0.f, 0.f, 0.12f);

		m_fade_left_image = make_ramp(m_fade_left_pixels, 256, true, 0.94f);
		m_fade_left.set_raw_image(m_fade_left_image.get());
		m_fade_left.set_size(820, virtual_height);

		m_fade_top_image = make_ramp(m_fade_top_pixels, 128, false, 0.6f);
		m_fade_top.set_raw_image(m_fade_top_image.get());
		m_fade_top.set_size(virtual_width, 130);

		// The bottom fade rises from the screen's foot: its ramp runs upward
		m_fade_bottom_image = make_ramp(m_fade_bottom_pixels, 256, false, 0.96f);
		std::reverse(reinterpret_cast<u32*>(m_fade_bottom_pixels.data()), reinterpret_cast<u32*>(m_fade_bottom_pixels.data()) + 256);
		m_fade_bottom.set_raw_image(m_fade_bottom_image.get());
		m_fade_bottom.set_size(virtual_width, 360);
		m_fade_bottom.set_pos(0, virtual_height - 360);

		// The Library's glow: a soft disc, tinted and stretched behind the covers
		{
			constexpr u16 n = 128;
			m_glow_pixels.resize(usz{n} * n * 4);
			for (u16 yy = 0; yy < n; yy++)
			{
				for (u16 xx = 0; xx < n; xx++)
				{
					const f32 dx = (xx + 0.5f) / n * 2.f - 1.f;
					const f32 dy = (yy + 0.5f) / n * 2.f - 1.f;
					const f32 r = std::min(1.f, std::sqrt(dx * dx + dy * dy));
					const f32 a = (1.f - r) * (1.f - r) * (1.f - r);
					u8* px = &m_glow_pixels[(usz{yy} * n + xx) * 4];
					px[0] = px[1] = px[2] = 255;
					px[3] = static_cast<u8>(std::lround(255.f * a));
				}
			}
			m_glow_image = std::make_unique<memory_image_info>(n, n, u8{4}, m_glow_pixels.data());
			m_glow_image->dirty = true;
		}
		m_library_art.set_size(virtual_width, virtual_height);
		m_library_art.back_color.a = 0.f;
		m_library_art.set_blur_strength(60);

		for (image_view* fade : {&m_fade_left, &m_fade_top, &m_fade_bottom})
		{
			fade->fore_color = c_backdrop;
			fade->back_color.a = 0.f;
		}

		// Top bar: the logo, a divider, the tabs, the user
		m_logo_data = load_image("/app0/assets/launcher/rpcs3-logo.png");
		if (m_logo_data)
		{
			// Drawn at three times the virtual space
			m_logo.set_raw_image(m_logo_data.get());
			m_logo.set_size(static_cast<u16>(m_logo_data->w / 3), static_cast<u16>(m_logo_data->h / 3));
			m_logo.back_color.a = 0.f;
			m_logo.set_pos(c_margin, static_cast<s16>(c_bar_y - m_logo.h / 2));
		}
		else
		{
			style_label(m_logo_text, "RPCS3", 15, f_bold, c_text);
			place(m_logo_text, c_margin, c_bar_y);
		}
		const s16 logo_right = m_logo_data ? static_cast<s16>(m_logo.x + m_logo.w) : static_cast<s16>(m_logo_text.x + m_logo_text.w);

		m_bar_divider.set_pos(static_cast<s16>(logo_right + 22), c_bar_y - 12);
		m_bar_divider.set_size(1, 24);
		m_bar_divider.back_color = color4f(1.f, 1.f, 1.f, 0.28f);

		for (const char* name : {"Home", "Library", "Settings"})
		{
			m_tab_labels.push_back(make_label(name, 13, f_medium, c_text_dim));
		}
		m_tab_underline.border_radius = 2;
		m_tab_underline.back_color = c_accent;

		const std::string user = read_user_name();
		style_label(m_user_name, user, 12, f_semibold, c_text);
		place(m_user_name, static_cast<s16>(c_right - m_user_name.w), c_bar_y);

		m_avatar.set_size(30, 30);
		m_avatar.set_pos(static_cast<s16>(m_user_name.x - 12 - 30), c_bar_y - 15);
		m_avatar.back_color = c_accent;

		// The initial, centred on the circle by its drawn shape, not its advance
		const std::u32string initial = utf8_to_u32string(user).substr(0, 1);
		std::u32string upper = initial;
		if (!upper.empty() && upper[0] < 0x80)
		{
			upper[0] = static_cast<char32_t>(std::toupper(static_cast<int>(upper[0])));
		}
		m_avatar_letter.set_font(13, f_bold);
		m_avatar_letter.set_unicode_text(upper);
		m_avatar_letter.fore_color = c_button_text;
		m_avatar_letter.back_color.a = 0.f;
		m_avatar_letter.set_padding(0);
		m_avatar_letter.auto_resize();
		{
			font* renderer = m_avatar_letter.get_font();
			const ink shape = measure_ink(renderer, upper);
			const f32 cx = m_avatar.x + m_avatar.w / 2.f;
			const f32 cy = m_avatar.y + m_avatar.h / 2.f;
			m_avatar_letter.set_pos(static_cast<s16>(std::lround(cx - (shape.left + shape.right) / 2.f)),
				static_cast<s16>(std::lround(cy - renderer->get_size_px() - (shape.top + shape.bottom) / 2.f)));
		}

		// Hero
		style_label(m_welcome, spaced("WELCOME BACK"), 10, f_semibold, c_text_dim);
		place(m_welcome, c_hero_x, 146);

		m_title.set_font(40, f_bold);
		m_title.fore_color = c_text;
		m_title.back_color.a = 0.f;
		m_title.set_padding(0);
		m_title.set_wrap_text(true);
		m_title.set_pos(c_hero_x - 2, 166);
		m_title.set_size(600, 120);

		m_play_button.set_size(176, 44);
		m_play_button.border_radius = 22;
		m_play_button.back_color = c_button;

		m_play_icon_data = resource_config::load_icon("home/32/play-button-arrowhead.png");
		m_play_icon.set_size(16, 16);
		m_play_icon.back_color.a = 0.f;
		if (m_play_icon_data)
		{
			m_play_icon_data->dirty = true;
			m_play_icon.set_raw_image(m_play_icon_data.get());
			m_play_icon.fore_color = c_button_text;
		}
		style_label(m_play_label, "Play now", 14, f_semibold, c_button_text);

		const auto round_button = [](ellipse& button, image_view& icon, std::unique_ptr<image_info>& data, const std::string& path, label& text, std::string_view caption)
		{
			button.set_size(44, 44);
			button.back_color = c_glass;
			button.border_size = 1;
			button.border_color = c_glass_border;
			data = path.starts_with("/") ? load_image(path) : resource_config::load_icon(path);
			icon.set_size(20, 20);
			icon.back_color.a = 0.f;
			if (data)
			{
				data->dirty = true;
				icon.set_raw_image(data.get());
			}
			style_label(text, caption, 12, f_medium, c_text);
		};
		round_button(m_settings_button, m_settings_icon, m_settings_icon_data, "home/32/settings.png", m_settings_label, "Game settings");
		round_button(m_delete_button, m_delete_icon, m_delete_icon_data, "/app0/assets/launcher/trash.png", m_delete_label, "Delete");

		// The delete confirmation, centred over a dimmed screen
		m_confirm_dim.set_size(virtual_width, virtual_height);
		m_confirm_dim.back_color = color4f(0.f, 0.f, 0.f, 0.62f);
		m_confirm_panel.set_size(600, 220);
		m_confirm_panel.set_pos((virtual_width - 600) / 2, (virtual_height - 220) / 2);
		m_confirm_panel.border_radius = 20;
		m_confirm_panel.back_color = color4f(0.06f, 0.07f, 0.12f, 0.98f);
		m_confirm_panel.border_size = 1;
		m_confirm_panel.border_color = color4f(1.f, 1.f, 1.f, 0.16f);
		style_label(m_confirm_title, "", 17, f_semibold, c_text);
		m_confirm_title.set_wrap_text(true);
		style_label(m_confirm_body, "", 12, f_regular, c_text_dim);
		m_confirm_body.set_wrap_text(true);

		const auto make_hint = [](hint& target, u8 image, std::string_view text)
		{
			target.icon.set_image_resource(image);
			target.icon.set_size(18, 18);
			target.icon.back_color.a = 0.f;
			style_label(target.text, text, 11, f_medium, c_text);
		};
		make_hint(m_confirm_yes, resource_config::confirm_button_resource(), "Delete");
		make_hint(m_confirm_no, resource_config::cancel_button_resource(), "Cancel");

		// The games row
		style_label(m_row_title, "Your games", 12, f_semibold, c_text);
		place(m_row_title, c_margin, c_row_y - 24);
		m_row_rule.set_pos(static_cast<s16>(c_margin + m_row_title.w + 18), c_row_y - 24);
		m_row_rule.set_size(static_cast<u16>(c_right - m_row_rule.x), 1);
		m_row_rule.back_color = color4f(1.f, 1.f, 1.f, 0.2f);

		// A rim: filled, behind the tile, which covers all but its edge. (An
		// outline-only rounded_rect leaks a hairline along its diagonal, where
		// its two triangles meet, across whatever it is drawn over)
		m_highlight.border_radius = c_tile_radius + 3;
		m_highlight.back_color = c_accent;
		m_highlight.set_size(c_tile_w + 6, c_tile_h + 6);

		style_label(m_placeholder, "Looking for games...", 13, f_regular, c_text_dim);
		place(m_placeholder, c_margin, c_row_y + 40);

		layout_tabs();
		layout_home();
	}

	void ps5_launcher_dialog::layout_tabs()
	{
		s16 x = static_cast<s16>(m_bar_divider.x + 30);
		for (usz i = 0; i < m_tab_labels.size(); i++)
		{
			label& tab_label = *m_tab_labels[i];
			const bool active = i == static_cast<usz>(m_tab);
			tab_label.set_font(13, active ? f_semibold : f_medium);
			tab_label.fore_color = active ? c_text : c_text_dim;
			tab_label.auto_resize();
			place(tab_label, x, c_bar_y);
			tab_label.refresh();

			if (active)
			{
				m_tab_underline.set_pos(static_cast<s16>(x - 10), c_bar_y + 16);
				m_tab_underline.set_size(static_cast<u16>(tab_label.w + 20), 3);
				m_tab_underline.refresh();
			}

			x = static_cast<s16>(x + tab_label.w + 34);
		}

		layout_hints();
	}

	void ps5_launcher_dialog::layout_hints()
	{
		// Right-aligned on one line: what the buttons do here
		std::vector<std::pair<u8, std::string_view>> hints;
		if (m_gs_open)
		{
			hints.emplace_back(resource_config::confirm_button_resource(), "Change");
			hints.emplace_back(static_cast<u8>(resource_config::standard_image_resource::square), "Use global");
			hints.emplace_back(resource_config::cancel_button_resource(), "Save and close");
		}
		else if (m_tab != tab::settings && !m_games.empty())
		{
			hints.emplace_back(resource_config::confirm_button_resource(), m_focus == focus::tiles ? "Play" : "Select");
			hints.emplace_back(static_cast<u8>(resource_config::standard_image_resource::triangle), "Settings");
			hints.emplace_back(static_cast<u8>(resource_config::standard_image_resource::square), "Delete");
		}
		else if (m_tab != tab::home)
		{
			hints.emplace_back(resource_config::confirm_button_resource(), "Select");
			hints.emplace_back(resource_config::cancel_button_resource(), "Back");
		}
		if (!m_gs_open)
		{
			hints.emplace_back(static_cast<u8>(resource_config::standard_image_resource::L1), "");
			hints.emplace_back(static_cast<u8>(resource_config::standard_image_resource::R1), "Tabs");
		}

		m_hints.clear();
		for (const auto& [image, text] : hints)
		{
			auto entry = std::make_unique<hint>();
			entry->icon.set_image_resource(image);
			entry->icon.set_size(20, 20);
			entry->icon.back_color.a = 0.f;
			style_label(entry->text, text, 11, f_medium, c_text_dim);
			m_hints.push_back(std::move(entry));
		}

		// A glyph with no words (L1) sits close to the next one (R1, "Tabs")
		s16 x = c_right;
		for (usz i = m_hints.size(); i-- > 0;)
		{
			hint& entry = *m_hints[i];
			if (!entry.text.text.empty())
			{
				x = static_cast<s16>(x - entry.text.w);
				place(entry.text, x, c_hints_y);
				x = static_cast<s16>(x - 7);
			}
			x = static_cast<s16>(x - 20);
			entry.icon.set_pos(x, c_hints_y - 10);
			x = static_cast<s16>(x - ((i > 0 && m_hints[i - 1]->text.text.empty()) ? 4 : 24));
		}
	}

	void ps5_launcher_dialog::layout_confirm()
	{
		const s16 left = static_cast<s16>(m_confirm_panel.x + 36);
		m_confirm_title.set_pos(left, static_cast<s16>(m_confirm_panel.y + 32));
		m_confirm_title.set_size(528, 60);
		m_confirm_title.auto_resize(false, 528, 60);
		m_confirm_body.set_pos(left, static_cast<s16>(m_confirm_title.y + m_confirm_title.h + 14));
		m_confirm_body.set_size(528, 80);
		m_confirm_body.auto_resize(false, 528, 80);

		const f32 hints_y = m_confirm_panel.y + m_confirm_panel.h - 36.f;
		s16 x = left;
		for (hint* entry : {&m_confirm_yes, &m_confirm_no})
		{
			entry->icon.set_pos(x, static_cast<s16>(hints_y - 9));
			entry->text.auto_resize();
			place(entry->text, static_cast<s16>(x + 26), hints_y);
			x = static_cast<s16>(x + 26 + entry->text.w + 28);
		}
	}

	void ps5_launcher_dialog::layout_home()
	{
		// The title's lines decide where the chips and the buttons go
		const ps5_launcher_game* game = (m_selected >= 0 && static_cast<usz>(m_selected) < m_games.size()) ? &m_games[m_selected] : nullptr;

		// Anchored at the buttons' line, as the design: the title grows upward
		// from the chips, and the greeting sits over its first line
		constexpr s16 buttons_y = 404;
		constexpr s16 chips_y = 350;

		m_title.set_text(game ? (game->info.name.empty() ? game->info.serial : game->info.name) : std::string(m_loading ? "" : "No games yet"));
		m_title.set_size(600, 120);
		m_title.auto_resize(false, 600, 120);
		{
			font* renderer = m_title.get_font();
			const ink cap = measure_ink(renderer, U"H");
			// The last line's baseline sits 20 above the chips
			const f32 last_baseline = (game ? chips_y : buttons_y) - 20.f;
			const f32 extra_lines = std::max(0.f, static_cast<f32>(m_title.h) - renderer->get_size_px());
			m_title.set_pos(c_hero_x - 2, static_cast<s16>(std::lround(last_baseline - extra_lines - renderer->get_size_px())));
			place(m_welcome, c_hero_x, m_title.y + renderer->get_size_px() + cap.top - 24.f);
			m_welcome.refresh();
		}
		s16 y = chips_y;

		// Chips: what the game's PARAM.SFO says of it
		m_chips.clear();
		m_chip_labels.clear();
		if (game)
		{
			std::vector<std::string> chips;
			if (game->info.category == "DG") chips.push_back("Disc");
			else if (game->info.category == "HG") chips.push_back("Digital");
			if (!game->info.serial.empty()) chips.push_back(game->info.serial);
			if (!game->info.app_ver.empty() && game->info.app_ver != "Unknown") chips.push_back("Version " + game->info.app_ver);

			s16 x = c_hero_x;
			for (const std::string& text : chips)
			{
				auto chip_label = make_label(text, 10, f_medium, c_text);
				auto chip = std::make_unique<rounded_rect>();
				chip->border_radius = 12;
				chip->back_color = c_glass;
				chip->border_size = 1;
				chip->border_color = color4f(1.f, 1.f, 1.f, 0.14f);
				chip->set_pos(x, y);
				chip->set_size(static_cast<u16>(chip_label->w + 30), 24);
				place(*chip_label, static_cast<s16>(x + 15), y + 12.f);
				x = static_cast<s16>(x + chip->w + 8);
				m_chips.push_back(std::move(chip));
				m_chip_labels.push_back(std::move(chip_label));
			}
		}
		y = buttons_y;

		const f32 mid = y + 22.f;
		m_play_button.set_pos(c_hero_x, y);
		m_play_icon.set_pos(static_cast<s16>(c_hero_x + 30), static_cast<s16>(mid - 8));
		place(m_play_label, static_cast<s16>(c_hero_x + 58), mid);

		m_settings_button.set_pos(static_cast<s16>(c_hero_x + m_play_button.w + 22), y);
		m_settings_icon.set_pos(static_cast<s16>(m_settings_button.x + 12), static_cast<s16>(y + 12));
		place(m_settings_label, static_cast<s16>(m_settings_button.x + 44 + 14), mid);

		m_delete_button.set_pos(static_cast<s16>(m_settings_label.x + m_settings_label.w + 30), y);
		m_delete_icon.set_pos(static_cast<s16>(m_delete_button.x + 12), static_cast<s16>(y + 12));
		place(m_delete_label, static_cast<s16>(m_delete_button.x + 44 + 14), mid);

		for (overlay_element* element : std::initializer_list<overlay_element*>{&m_title, &m_play_button, &m_play_icon, &m_play_label, &m_settings_button,
				&m_settings_icon, &m_settings_label, &m_delete_button, &m_delete_icon, &m_delete_label})
		{
			element->refresh();
		}

		// The background: the game's art, or its icon blurred, or none; the last
		// one stays beneath while the new one fades in
		const image_info_base* next = game ? (game->background ? game->background.get() : game->icon.get()) : nullptr;
		if (next != m_background_image)
		{
			m_background_fading = m_background_image && next;
			if (m_background_fading)
			{
				m_background_prev.set_raw_image(m_background_image);
				m_background_prev.set_blur_strength(m_background_blur);
				m_background_prev.fore_color = color4f(1.f);
				m_background_prev.refresh();
				m_background_fade_start = 0; // set by the next update
			}

			m_background_image = next;
			m_background_blur = (game && !game->background) ? 80 : 0;
			if (next)
			{
				m_background.set_blur_strength(m_background_blur);
				m_background.set_raw_image(next);
				m_background.fore_color = color4f(1.f, 1.f, 1.f, m_background_fading ? 0.f : 1.f);
				m_background.refresh();
			}
		}

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
			const s16 x = static_cast<s16>(c_margin + i * (c_tile_w + c_tile_gap));
			const bool selected = m_first_visible + i == m_selected;

			auto tile = std::make_unique<rounded_image>();
			tile->border_radius = c_tile_radius;
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

			auto name = make_label("", 12, selected ? f_semibold : f_medium, selected ? c_text : c_text_dim);
			fit_text(*name, entry.info.name.empty() ? entry.info.serial : entry.info.name, c_tile_w - 4);
			place(*name, static_cast<s16>(x + 2), c_row_y + c_tile_h + 22.f);

			if (selected)
			{
				m_highlight.set_pos(static_cast<s16>(x - 3), static_cast<s16>(c_row_y - 3));
				m_highlight.refresh();
			}

			m_tiles.push_back(std::move(tile));
			m_tile_labels.push_back(std::move(name));
		}

		if (!m_loading)
		{
			style_label(m_placeholder, "No games found. Put each game's folder in /data/homebrew/PPSA99200/rpcs3/games/", 13, f_regular, c_text_dim);
			place(m_placeholder, c_margin, c_row_y + 40);
		}

		layout_focus();
	}

	void ps5_launcher_dialog::layout_focus()
	{
		// The focused button gets the accent's ring; the tile ring is bright
		// while the row has the focus and faint while a button has it
		const auto outline = [](overlay_element& button, bool focused, u8 idle_border, const color4f& idle_color)
		{
			button.border_size = focused ? 3 : idle_border;
			button.border_color = focused ? c_accent : idle_color;
			button.refresh();
		};
		outline(m_play_button, m_focus == focus::play, 0, c_glass_border);
		outline(m_settings_button, m_focus == focus::settings, 1, c_glass_border);
		outline(m_delete_button, m_focus == focus::remove, 1, c_glass_border);

		m_highlight.back_color = m_focus == focus::tiles ? c_accent : color4f(c_accent.r, c_accent.g, c_accent.b, 0.3f);
		m_highlight.refresh();

		layout_hints();
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
		m_confirm_yes.text.set_text("Delete");
		m_confirm_no.icon.set_visible(true);
		m_confirm_no.text.set_visible(true);
		layout_confirm();
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
		layout_confirm();

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
				if (const std::string config = rpcs3::utils::get_custom_config_path(info.serial); fs::is_file(config))
				{
					fs::remove_file(config);
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
					m_confirm_yes.text.set_text("OK");
					m_confirm_no.icon.set_visible(false);
					m_confirm_no.text.set_visible(false);
					layout_confirm();
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
				load_cover(game);
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
			// The art shown belongs to the list being replaced
			m_background_image = nullptr;
			m_background_fading = false;
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
			// The row starts where the selection is
			m_flow_pos = static_cast<f32>(m_selected);
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


	namespace
	{
		// The settings a game may want of its own, as the config file names them
		struct game_setting_spec
		{
			const char* section;
			const char* key;
			const char* label;
			const char* help;
			std::vector<std::string> options; // empty: every value the setting has
		};

		const std::vector<game_setting_spec>& game_setting_specs()
		{
			static const std::vector<game_setting_spec> specs
			{
				{"", "", "PROCESSOR", "", {}},
				{"Core", "PPU Decoder", "PPU decoder", "How the code of the PS3's main processor runs. The recompiler is far faster; the interpreter is only for tracking down a fault.", {}},
				{"Core", "SPU Decoder", "SPU decoder", "How the code of the SPUs runs. LLVM is the fastest; the others are slower fallbacks for a game that breaks with it.", {}},
				{"Core", "SPU Block Size", "SPU block size", "How much SPU code is compiled at once. Mega and Giga can run faster but compile longer; Safe works with every game.", {}},
				{"Core", "SPU XFloat Accuracy", "SPU float accuracy", "How exactly the SPUs' floating point is followed. Raise it for broken lighting, physics or animation; lower is faster.", {}},
				{"Core", "Preferred SPU Threads", "Preferred SPU threads", "How many SPU threads may run at the same time. Auto leaves it to RPCS3; some heavy games run better with 1 or 2.", {"0", "1", "2", "3", "4", "5", "6"}},
				{"Core", "Max SPURS Threads", "Max SPURS threads", "Caps the threads a game's SPURS task system may use. Lower can help heavy games; too low can stutter.", {"1", "2", "3", "4", "5", "6"}},
				{"Core", "SPU loop detection", "SPU loop detection", "Lets the processor go when an SPU waits in a loop. Can make a game faster, or break timing in a few.", {}},
				{"", "", "GRAPHICS", "", {}},
				{"Video", "Resolution Scale", "Resolution scale", "Draws the game at a multiple of its own resolution. Above 100% is sharper and costs GPU time.", {"50", "75", "100", "125", "150", "200", "250", "300"}},
				{"Video", "Frame limit", "Frame limit", "Caps the frame rate. Auto follows the game; Off can make a game run too fast.", {}},
				{"Video", "Shader Mode", "Shader mode", "How new shaders are built. Async builds them on other threads, so a new effect waits a moment instead of the whole game. (The shader interpreter froze the console and is left out.)",
					{"Async Recompiler (multi-threaded)", "Legacy Recompiler (single-threaded)"}},
				{"Video", "Resolution", "Resolution", "The output resolution the game is told the PS3 has. Most games want 720p.", {}},
				{"Video", "Anisotropic Filter Override", "Anisotropic filtering", "Sharpens textures seen at an angle. Auto keeps the game's own setting.", {"0", "2", "4", "8", "16"}},
				{"Video", "Write Color Buffers", "Write color buffers", "Copies what is drawn back to the PS3's memory. Fixes missing effects in some games, at a cost.", {}},
				{"Video", "Strict Rendering Mode", "Strict rendering", "Follows the PS3's rendering rules more closely. Fixes some graphical errors and is slower.", {}},
				{"Video", "Multithreaded RSX", "Multithreaded RSX", "Moves part of the graphics work to a thread of its own. Can help games that are short of processor time.", {}},
				{"Video", "Relaxed ZCULL Sync", "Relaxed ZCULL sync", "Looser timing for occlusion queries. Faster in games that use many; things may flicker.", {}},
				{"Video", "Disable ZCull Occlusion Queries", "Skip occlusion queries", "Answers occlusion queries without drawing them. Faster; objects may pop in or vanish.", {}},
				{"", "", "AUDIO", "", {}},
				{"Audio", "Master Volume", "Volume", "The game's volume.", {"25", "50", "75", "100", "125", "150", "200"}},
				{"Audio", "Enable Time Stretching", "Time stretching", "Stretches the sound instead of letting it crackle when the game runs slow.", {}},
			};
			return specs;
		}

		cfg::_base* find_setting(cfg::node& root, std::string_view section, std::string_view key)
		{
			for (cfg::_base* node : root.get_nodes())
			{
				if (node->get_type() != cfg::type::node || node->get_name() != section)
				{
					continue;
				}
				for (cfg::_base* setting : static_cast<cfg::node*>(node)->get_nodes())
				{
					if (setting->get_name() == key)
					{
						return setting;
					}
				}
			}
			return nullptr;
		}

		std::string setting_text(const std::string& key, const std::string& value)
		{
			if (value == "true") return "On";
			if (value == "false") return "Off";
			if (value == "0" && (key == "Preferred SPU Threads" || key == "Anisotropic Filter Override")) return "Auto";
			if (key == "Resolution Scale" || key == "Master Volume") return value + "%";
			if (key == "Anisotropic Filter Override") return value + "x";
			return value;
		}

		// The global config, as a game's boot starts from: the defaults, then config.yml
		std::unique_ptr<cfg_root> load_global_config()
		{
			auto root = std::make_unique<cfg_root>();
			root->from_string(g_cfg_defaults);
			if (fs::file file{fs::get_config_dir(true) + "config.yml"})
			{
				root->from_string(file.to_string());
			}
			return root;
		}

		std::string yaml_quoted(std::string_view text)
		{
			std::string result = "\"";
			for (const char c : text)
			{
				if (c == '"' || c == '\\') result += '\\';
				result += c;
			}
			return result + "\"";
		}

		// The entries of `config` that differ from `base`, as YAML: what a custom
		// config needs to hold, and nothing the global config already says
		bool write_differences(std::string& out, const cfg::node& config, const cfg::node& base, int depth)
		{
			bool any = false;
			const auto& nodes = config.get_nodes();
			const auto& base_nodes = base.get_nodes();
			for (usz i = 0; i < nodes.size() && i < base_nodes.size(); i++)
			{
				const cfg::_base* entry = nodes[i];
				const cfg::_base* base_entry = base_nodes[i];
				const std::string indent(depth * 2, ' ');

				switch (entry->get_type())
				{
				case cfg::type::node:
				{
					std::string inner;
					if (write_differences(inner, *static_cast<const cfg::node*>(entry), *static_cast<const cfg::node*>(base_entry), depth + 1))
					{
						out += indent + yaml_quoted(entry->get_name()) + ":\n" + inner;
						any = true;
					}
					break;
				}
				case cfg::type::_bool:
				case cfg::type::_enum:
				case cfg::type::_int:
				case cfg::type::uint:
				case cfg::type::string:
				{
					if (const std::string value = entry->to_string(); value != base_entry->to_string())
					{
						out += indent + yaml_quoted(entry->get_name()) + ": " + yaml_quoted(value) + "\n";
						any = true;
					}
					break;
				}
				default:
					break;
				}
			}
			return any;
		}
	}

	void ps5_launcher_dialog::open_game_settings()
	{
		if (m_selected < 0 || static_cast<usz>(m_selected) >= m_games.size())
		{
			return;
		}

		const big_picture_game_info& info = m_games[m_selected].info;
		if (info.serial.empty())
		{
			return;
		}

		m_gs_serial = info.serial;
		m_gs_name = info.name.empty() ? info.serial : info.name;

		// What the game gets today: the global config, then its custom config
		const auto global = load_global_config();
		const auto game = load_global_config();
		if (fs::file file{rpcs3::utils::get_custom_config_path(m_gs_serial)})
		{
			game->from_string(file.to_string());
		}

		m_gs_rows.clear();
		for (const game_setting_spec& spec : game_setting_specs())
		{
			game_setting row;
			row.label = spec.label;
			if (!*spec.section)
			{
				row.heading = true;
				m_gs_rows.push_back(std::move(row));
				continue;
			}

			cfg::_base* global_setting = find_setting(*global, spec.section, spec.key);
			cfg::_base* game_setting_entry = find_setting(*game, spec.section, spec.key);
			if (!global_setting || !game_setting_entry)
			{
				launcher_log.error("Game settings: no setting %s/%s", spec.section, spec.key);
				continue;
			}

			row.section = spec.section;
			row.key = spec.key;
			row.help = spec.help;
			row.global = global_setting->to_string();
			row.value = game_setting_entry->to_string();
			row.options = spec.options.empty() ? global_setting->to_list() : spec.options;
			for (const std::string& value : {row.global, row.value})
			{
				if (std::find(row.options.begin(), row.options.end(), value) == row.options.end())
				{
					row.options.insert(row.options.begin(), value);
				}
			}
			m_gs_rows.push_back(std::move(row));
		}

		m_gs_selected = 1;
		m_gs_scroll = 0;
		m_gs_open = true;
		m_gs_open_us = m_now_us;
		play_sound(sound_effect::accept);
		layout_game_settings();
		layout_hints();
	}

	void ps5_launcher_dialog::close_game_settings()
	{
		// The game's custom config: what differs from the global config, among
		// these settings and any the file held already
		const auto global = load_global_config();
		const auto game = load_global_config();
		const std::string path = rpcs3::utils::get_custom_config_path(m_gs_serial);
		if (fs::file file{path})
		{
			game->from_string(file.to_string());
		}
		for (const game_setting& row : m_gs_rows)
		{
			if (!row.heading)
			{
				if (cfg::_base* setting = find_setting(*game, row.section, row.key))
				{
					setting->from_string(row.value);
				}
			}
		}

		std::string yaml;
		if (write_differences(yaml, *game, *global, 0))
		{
			fs::create_path(rpcs3::utils::get_custom_config_dir());
			fs::pending_file temp(path);
			if (temp.file)
			{
				temp.file.write(yaml);
			}
			if (!temp.file || !temp.commit())
			{
				launcher_log.error("Game settings: could not write %s (%s)", path, fs::g_tls_error);
			}
			else
			{
				launcher_log.notice("Game settings: saved %s", path);
			}
		}
		else if (fs::is_file(path))
		{
			// Nothing of its own left: the game follows the global config
			fs::remove_file(path);
			launcher_log.notice("Game settings: removed %s", path);
		}

		m_gs_open = false;
		m_gs_items.clear();
		play_sound(sound_effect::cancel);
		layout_hints();
	}

	void ps5_launcher_dialog::handle_game_settings(pad_button button_press)
	{
		const bool up = button_press == pad_button::dpad_up || button_press == pad_button::ls_up;
		const bool down = button_press == pad_button::dpad_down || button_press == pad_button::ls_down;
		const bool left = button_press == pad_button::dpad_left || button_press == pad_button::ls_left;
		const bool right = button_press == pad_button::dpad_right || button_press == pad_button::ls_right || button_press == pad_button::cross;

		if (button_press == pad_button::circle || button_press == pad_button::triangle)
		{
			close_game_settings();
			return;
		}

		if (up || down)
		{
			// The next row that is a setting, not a heading
			s32 next = m_gs_selected;
			do
			{
				next += up ? -1 : 1;
			}
			while (next >= 0 && next < static_cast<s32>(m_gs_rows.size()) && m_gs_rows[next].heading);

			if (next >= 0 && next < static_cast<s32>(m_gs_rows.size()))
			{
				m_gs_selected = next;
				play_sound(sound_effect::cursor);
				layout_game_settings();
			}
			return;
		}

		if (m_gs_selected < 0 || static_cast<usz>(m_gs_selected) >= m_gs_rows.size())
		{
			return;
		}
		game_setting& row = m_gs_rows[m_gs_selected];

		if (left || right)
		{
			const auto it = std::find(row.options.begin(), row.options.end(), row.value);
			const s32 at = static_cast<s32>(it - row.options.begin());
			const s32 count = static_cast<s32>(row.options.size());
			row.value = row.options[(at + (left ? count - 1 : 1)) % count];
			play_sound(sound_effect::cursor);
			layout_game_settings();
		}
		else if (button_press == pad_button::square && row.value != row.global)
		{
			row.value = row.global;
			play_sound(sound_effect::cancel);
			layout_game_settings();
		}
	}

	void ps5_launcher_dialog::layout_game_settings()
	{
		m_gs_items.clear();
		const auto add = [this](std::unique_ptr<overlay_element> item) -> overlay_element&
		{
			m_gs_items.push_back(std::move(item));
			return *m_gs_items.back();
		};

		// A veil over the art, deeper on the left where the list is
		auto veil = std::make_unique<overlay_element>();
		veil->set_size(virtual_width, virtual_height);
		veil->back_color = color4f(c_backdrop.r, c_backdrop.g, c_backdrop.b, 0.55f);
		add(std::move(veil));

		// Heading
		auto kicker = make_label(spaced("GAME SETTINGS"), 10, f_semibold, c_text_dim);
		place(*kicker, c_hero_x, 104);
		add(std::move(kicker));

		auto title = make_label("", 24, f_bold, c_text);
		fit_text(*title, m_gs_name, 760);
		place(*title, c_hero_x, 136);
		add(std::move(title));

		auto note = make_label("Saved for this game only, and used the next time it starts. Marked settings differ from the global ones.", 11, f_regular, c_text_dim);
		place(*note, c_hero_x, 166);
		add(std::move(note));

		// The list: rows from y 196 to 646, scrolled to keep the selection in view
		constexpr s16 list_x = c_hero_x - 12;
		constexpr u16 list_w = 760;
		constexpr s16 list_top = 192;
		constexpr s16 list_bottom = 650;
		constexpr s16 row_h = 38;
		constexpr s16 heading_h = 34;

		std::vector<s16> tops;
		s16 y = 0;
		for (const game_setting& row : m_gs_rows)
		{
			tops.push_back(y);
			y = static_cast<s16>(y + (row.heading ? heading_h : row_h));
		}
		if (m_gs_selected >= 0 && static_cast<usz>(m_gs_selected) < tops.size())
		{
			const s16 top = tops[m_gs_selected];
			const s16 visible = list_bottom - list_top;
			// Keep the heading above the first setting in view
			const s16 want_top = m_gs_selected > 0 && m_gs_rows[m_gs_selected - 1].heading ? tops[m_gs_selected - 1] : top;
			if (want_top < m_gs_scroll) m_gs_scroll = want_top;
			if (top + row_h > m_gs_scroll + visible) m_gs_scroll = top + row_h - visible;
		}

		for (usz i = 0; i < m_gs_rows.size(); i++)
		{
			const game_setting& row = m_gs_rows[i];
			const s16 top = static_cast<s16>(list_top + tops[i] - m_gs_scroll);
			const s16 h = row.heading ? heading_h : row_h;
			if (top < list_top || top + h > list_bottom)
			{
				continue;
			}

			if (row.heading)
			{
				auto heading = make_label(spaced(row.label), 9, f_semibold, c_accent);
				place(*heading, static_cast<s16>(list_x + 12), top + 22.f);
				add(std::move(heading));
				continue;
			}

			const bool selected = static_cast<s32>(i) == m_gs_selected;
			const bool own = row.value != row.global;
			const f32 mid = top + h / 2.f;

			if (selected)
			{
				auto bar = std::make_unique<rounded_rect>();
				bar->set_pos(list_x, static_cast<s16>(top + 2));
				bar->set_size(list_w, static_cast<u16>(h - 4));
				bar->border_radius = 10;
				bar->back_color = color4f(1.f, 1.f, 1.f, 0.1f);
				bar->border_size = 2;
				bar->border_color = c_accent;
				add(std::move(bar));
			}
			else
			{
				auto rule = std::make_unique<overlay_element>();
				rule->set_pos(static_cast<s16>(list_x + 12), static_cast<s16>(top + h - 1));
				rule->set_size(static_cast<u16>(list_w - 24), 1);
				rule->back_color = color4f(1.f, 1.f, 1.f, 0.07f);
				add(std::move(rule));
			}

			if (own)
			{
				auto dot = std::make_unique<ellipse>();
				dot->set_size(6, 6);
				dot->set_pos(static_cast<s16>(list_x + 12), static_cast<s16>(mid - 3));
				dot->back_color = c_accent;
				add(std::move(dot));
			}

			auto name = make_label(row.label, 13, selected ? f_semibold : f_medium, selected || own ? c_text : c_text_dim);
			place(*name, static_cast<s16>(list_x + 26), mid);
			add(std::move(name));

			// The value, right-aligned, with arrows on the selected row
			auto value = make_label(setting_text(row.key, row.value), 13, own ? f_semibold : f_medium, own ? c_accent : (selected ? c_text : c_text_dim));
			const s16 value_right = static_cast<s16>(list_x + list_w - (selected ? 34 : 18));
			place(*value, static_cast<s16>(value_right - value->w), mid);
			const s16 value_x = value->x;
			add(std::move(value));

			if (selected)
			{
				auto less = make_label("‹", 15, f_semibold, c_text);
				place(*less, static_cast<s16>(value_x - 16), mid);
				add(std::move(less));
				auto more = make_label("›", 15, f_semibold, c_text);
				place(*more, static_cast<s16>(value_right + 10), mid);
				add(std::move(more));
			}
		}

		// The selected setting, explained, on the right
		if (m_gs_selected >= 0 && static_cast<usz>(m_gs_selected) < m_gs_rows.size() && !m_gs_rows[m_gs_selected].heading)
		{
			const game_setting& row = m_gs_rows[m_gs_selected];
			constexpr s16 panel_x = 852;
			constexpr u16 panel_w = 388;

			auto panel = std::make_unique<rounded_rect>();
			panel->set_pos(panel_x, list_top);
			panel->border_radius = 16;
			panel->back_color = color4f(0.03f, 0.04f, 0.09f, 0.72f);
			panel->border_size = 1;
			panel->border_color = c_glass_border;
			overlay_element& panel_ref = add(std::move(panel));

			auto name = make_label(row.label, 15, f_semibold, c_text);
			place(*name, static_cast<s16>(panel_x + 24), list_top + 34.f);
			add(std::move(name));

			auto help = make_label("", 12, f_regular, c_text_dim);
			help->set_wrap_text(true);
			help->set_text(row.help);
			help->set_pos(static_cast<s16>(panel_x + 24), static_cast<s16>(list_top + 54));
			help->set_size(panel_w - 48, 120);
			help->auto_resize(false, panel_w - 48, 120);
			const s16 help_bottom = static_cast<s16>(help->y + help->h);
			add(std::move(help));

			auto rule = std::make_unique<overlay_element>();
			rule->set_pos(static_cast<s16>(panel_x + 24), static_cast<s16>(help_bottom + 18));
			rule->set_size(panel_w - 48, 1);
			rule->back_color = color4f(1.f, 1.f, 1.f, 0.12f);
			add(std::move(rule));

			const f32 line1 = help_bottom + 42.f;
			const f32 line2 = line1 + 26.f;
			for (const auto& [caption, text, f32_y, accent] : {std::tuple{"Global", setting_text(row.key, row.global), line1, false},
					std::tuple{"This game", setting_text(row.key, row.value), line2, row.value != row.global}})
			{
				auto left_label = make_label(caption, 12, f_medium, c_text_dim);
				place(*left_label, static_cast<s16>(panel_x + 24), f32_y);
				add(std::move(left_label));
				auto right_label = make_label(text, 12, f_semibold, accent ? c_accent : c_text);
				place(*right_label, static_cast<s16>(panel_x + panel_w - 24 - right_label->w), f32_y);
				add(std::move(right_label));
			}

			panel_ref.set_size(panel_w, static_cast<u16>(line2 + 26 - list_top));
			panel_ref.refresh();
		}
	}

	f32 ps5_launcher_dialog::intro_seconds() const
	{
		if (!m_play_intro)
		{
			return 100.f;
		}
		return m_intro_start_us ? (m_now_us - m_intro_start_us) / 1'000'000.f : 0.f;
	}

	f32 ps5_launcher_dialog::content_seconds() const
	{
		// From the later of the list being read and the splash making way
		const f32 since_read = m_content_start_us ? (m_now_us - m_content_start_us) / 1'000'000.f : -1.f;
		return std::min(since_read, intro_seconds() - c_intro_content);
	}

	bool ps5_launcher_dialog::intro_running() const
	{
		return intro_seconds() < c_intro_end || (m_content_start_us && content_seconds() < c_content_end);
	}

	void ps5_launcher_dialog::skip_intro()
	{
		// Straight to the end: both clocks run back past their last step
		constexpr u64 far = 30'000'000;
		if (m_intro_start_us && m_now_us > far)
		{
			m_intro_start_us = m_now_us - far;
		}
		if (m_content_start_us && m_now_us > far)
		{
			m_content_start_us = m_now_us - far;
		}
	}


	void ps5_launcher_dialog::handle_library(pad_button button_press)
	{
		if (m_games.empty())
		{
			return;
		}

		switch (button_press)
		{
		case pad_button::dpad_left:
		case pad_button::ls_left:
			select_game(m_selected - 1);
			break;
		case pad_button::dpad_right:
		case pad_button::ls_right:
			select_game(m_selected + 1);
			break;
		case pad_button::L2:
			select_game(m_selected - 5);
			break;
		case pad_button::R2:
			select_game(m_selected + 5);
			break;
		case pad_button::cross:
			boot_selected();
			break;
		case pad_button::triangle:
			open_game_settings();
			break;
		case pad_button::square:
			ask_delete();
			break;
		case pad_button::circle:
			set_tab(tab::home);
			break;
		default:
			break;
		}
	}

	void ps5_launcher_dialog::compile_library(compiled_resource& result)
	{
		// The selected game's art, blurred and dim, under a glow in the app's
		// colours: a wide blue one and a cyan heart behind the middle cover
		if (m_background_image)
		{
			m_library_art.set_raw_image(m_background_image);
			m_library_art.fore_color = color4f(1.f, 1.f, 1.f, 0.16f);
			m_library_art.refresh();
			result.add(m_library_art.get_compiled());
		}
		const auto glow = [&](s16 cx, s16 cy, u16 w, u16 h, const color4f& color)
		{
			image_view view;
			view.set_raw_image(m_glow_image.get());
			view.back_color.a = 0.f;
			view.fore_color = color;
			view.set_pos(static_cast<s16>(cx - w / 2), static_cast<s16>(cy - h / 2));
			view.set_size(w, h);
			result.add(view.get_compiled());
		};
		glow(640, 340, 1500, 900, color4f(0.16f, 0.3f, 0.8f, 0.32f));
		glow(640, 330, 720, 600, color4f(c_accent.r, c_accent.g, c_accent.b, 0.16f));

		if (m_games.empty())
		{
			result.add(m_placeholder.get_compiled());
			return;
		}

		// The row: the middle cover faces the screen; the others turn away to
		// each side, smaller and darker, the further the more stacked
		constexpr f32 centre_x = 640.f;
		constexpr f32 centre_y = 330.f;
		constexpr f32 cover_w = 300.f;
		constexpr f32 cover_h = cover_w * c_cover_h / c_cover_w;
		constexpr int strips = 14;

		struct placement
		{
			f32 x_left, x_right; // the edges on screen
			f32 h_left, h_right; // their heights
			f32 shade;           // brightness
			f32 alpha;
		};

		const auto place_cover = [&](f32 d) -> placement
		{
			const f32 side = d < 0.f ? -1.f : 1.f;
			const f32 a = std::abs(d);
			const f32 t = std::min(a, 1.f);
			const f32 e = t * t * (3.f - 2.f * t);

			const f32 scale = 1.f - 0.14f * e;
			const f32 width = cover_w * scale * (1.f - 0.32f * e);
			const f32 skew = 0.05f * e; // the far edge's loss of height, each end
			const f32 offset = 300.f * e + std::max(0.f, a - 1.f) * 88.f;
			const f32 mid_x = centre_x + side * offset;

			placement p{};
			p.x_left = mid_x - width / 2.f;
			p.x_right = mid_x + width / 2.f;
			const f32 near_h = cover_h * scale;
			const f32 far_h = near_h * (1.f - 2.f * skew);
			// The edge nearer the middle stays tall
			p.h_left = side > 0.f ? near_h : far_h;
			p.h_right = side > 0.f ? far_h : near_h;
			p.shade = 1.f - 0.45f * e - 0.06f * std::max(0.f, a - 1.f);
			p.alpha = std::clamp(4.6f - a, 0.f, 1.f);
			return p;
		};

		// A strip mesh of the cover between its edges, its texture spread as a
		// turned plane spreads it (perspective-correct across the width)
		const auto add_face = [&](const image_info_base* image, const placement& p, f32 y_mid, f32 v_top, f32 v_bottom, f32 top_frac, f32 bottom_frac, const color4f& color, bool mirrored)
		{
			compiled_resource::command cmd;
			cmd.config.set_image_resource(image_resource_id::raw_image);
			cmd.config.external_data_ref = image;
			cmd.config.color = color;
			cmd.config.primitives = primitive_type::triangle_strip;
			cmd.config.disable_vertex_snap = true;

			for (int i = 0; i <= strips; i++)
			{
				const f32 s = static_cast<f32>(i) / strips;
				const f32 x = p.x_left + (p.x_right - p.x_left) * s;
				const f32 h = p.h_left + (p.h_right - p.h_left) * s;
				const f32 u = s * p.h_right / ((1.f - s) * p.h_left + s * p.h_right);
				// This face spans [top_frac, bottom_frac] of the cover's height,
				// measured from its middle line
				const f32 y0 = mirrored ? y_mid + h / 2.f + h * top_frac : y_mid - h / 2.f + h * top_frac;
				const f32 y1 = mirrored ? y_mid + h / 2.f + h * bottom_frac : y_mid - h / 2.f + h * bottom_frac;
				vertex top, bottom;
				top.vec4(x, y0, u, v_top);
				bottom.vec4(x, y1, u, v_bottom);
				cmd.verts.push_back(top);
				cmd.verts.push_back(bottom);
			}

			compiled_resource part;
			part.append(cmd);
			result.add(part);
		};

		// Far ones first, the middle one last
		std::vector<s32> order;
		for (s32 i = 0; i < static_cast<s32>(m_games.size()); i++)
		{
			if (std::abs(i - m_flow_pos) < 5.f)
			{
				order.push_back(i);
			}
		}
		std::sort(order.begin(), order.end(), [&](s32 a, s32 b) { return std::abs(a - m_flow_pos) > std::abs(b - m_flow_pos); });

		for (const s32 i : order)
		{
			const ps5_launcher_game& game = m_games[i];
			const image_info_base* cover = game.cover();
			if (!cover)
			{
				continue;
			}

			const f32 d = i - m_flow_pos;
			const placement p = place_cover(d);
			if (p.alpha <= 0.f)
			{
				continue;
			}

			// The reflection on the floor: the cover's foot, mirrored, fading
			// out in bands
			constexpr int bands = 6;
			constexpr f32 depth = 0.3f;
			for (int b = 0; b < bands; b++)
			{
				const f32 f0 = depth * b / bands;
				const f32 f1 = depth * (b + 1) / bands;
				const f32 fade = 0.2f * (1.f - (b + 0.5f) / bands);
				add_face(cover, p, centre_y + 4.f, 1.f - f0, 1.f - f1, f0, f1, color4f(p.shade, p.shade, p.shade, fade * p.alpha), true);
			}

			// The glow and the ring of the middle one
			const f32 focus = std::clamp(1.f - std::abs(d) * 2.f, 0.f, 1.f);
			if (focus > 0.f)
			{
				const f32 w = p.x_right - p.x_left;
				const f32 h = std::max(p.h_left, p.h_right);
				for (int g = 3; g >= 1; g--)
				{
					rounded_rect halo;
					halo.set_pos(static_cast<s16>(p.x_left - g * 6), static_cast<s16>(centre_y - h / 2.f - g * 6));
					halo.set_size(static_cast<u16>(w + g * 12), static_cast<u16>(h + g * 12));
					halo.border_radius = static_cast<u16>(10 + g * 6);
					halo.back_color = color4f(c_accent.r, c_accent.g, c_accent.b, 0.07f * focus);
					result.add(halo.get_compiled());
				}
			}

			if (focus > 0.f)
			{
				// The rim: filled, behind the face (see m_highlight)
				rounded_rect rim;
				const f32 w = p.x_right - p.x_left;
				const f32 h = std::max(p.h_left, p.h_right);
				rim.set_pos(static_cast<s16>(std::lround(p.x_left - 3)), static_cast<s16>(std::lround(centre_y - h / 2.f - 3)));
				rim.set_size(static_cast<u16>(std::lround(w + 6)), static_cast<u16>(std::lround(h + 6)));
				rim.border_radius = 6;
				rim.back_color = color4f(c_accent.r, c_accent.g, c_accent.b, focus);
				result.add(rim.get_compiled());
			}

			add_face(cover, p, centre_y, 0.f, 1.f, 0.f, 1.f, color4f(p.shade, p.shade, p.shade, p.alpha), false);
		}

		// The selected game: its name and what it is, under the row
		if (m_selected >= 0 && static_cast<usz>(m_selected) < m_games.size())
		{
			const big_picture_game_info& info = m_games[m_selected].info;

			label name;
			style_label(name, "", 22, f_semibold, c_text);
			fit_text(name, info.name.empty() ? info.serial : info.name, 1000);
			place(name, static_cast<s16>(640 - name.w / 2), 586.f);
			result.add(name.get_compiled());

			std::vector<std::string> facts;
			if (!info.serial.empty()) facts.push_back(info.serial);
			if (const std::string region = region_of(info.serial); !region.empty()) facts.push_back(region);
			if (info.category == "DG") facts.push_back("Disc");
			else if (info.category == "HG") facts.push_back("Digital");
			if (!info.app_ver.empty() && info.app_ver != "Unknown") facts.push_back("v" + info.app_ver);
			const bool own_settings = !info.serial.empty() && fs::is_file(rpcs3::utils::get_custom_config_path(info.serial));

			// The facts, then a chip when the game has settings of its own,
			// centred together
			std::vector<std::unique_ptr<label>> parts;
			f32 total = 0.f;
			for (usz i = 0; i < facts.size(); i++)
			{
				if (i)
				{
					parts.push_back(make_label("\u00b7", 12, f_medium, c_text_dim));
				}
				parts.push_back(make_label(facts[i], 12, f_medium, c_text_dim));
			}
			for (usz i = 0; i < parts.size(); i++)
			{
				total += parts[i]->w + (i ? 10 : 0);
			}
			std::unique_ptr<label> chip_text;
			if (own_settings)
			{
				chip_text = make_label("Own settings", 10, f_semibold, c_accent);
				total += 16 + chip_text->w + 24;
			}

			f32 x = 640.f - total / 2.f;
			for (usz i = 0; i < parts.size(); i++)
			{
				place(*parts[i], static_cast<s16>(std::lround(x)), 620.f);
				result.add(parts[i]->get_compiled());
				x += parts[i]->w + (i + 1 < parts.size() ? 10 : 0);
			}
			if (chip_text)
			{
				x += 16;
				rounded_rect chip;
				chip.set_pos(static_cast<s16>(std::lround(x)), 608);
				chip.set_size(static_cast<u16>(chip_text->w + 24), 24);
				chip.border_radius = 12;
				chip.back_color = color4f(c_accent.r, c_accent.g, c_accent.b, 0.12f);
				chip.border_size = 1;
				chip.border_color = color4f(c_accent.r, c_accent.g, c_accent.b, 0.5f);
				result.add(chip.get_compiled());
				place(*chip_text, static_cast<s16>(std::lround(x + 12)), 620.f);
				result.add(chip_text->get_compiled());
			}

			// Where in the list, bottom left
			label count;
			style_label(count, fmt::format("%d / %d", m_selected + 1, m_games.size()), 11, f_medium, c_text_dim);
			place(count, c_margin, c_hints_y);
			result.add(count.get_compiled());
		}
	}

	void ps5_launcher_dialog::update(u64 timestamp_us)
	{
		{
			std::lock_guard lock(m_mutex);
			m_now_us = timestamp_us;
			if (!m_intro_start_us)
			{
				m_intro_start_us = timestamp_us;
			}
			if (!m_content_start_us && !m_loading)
			{
				m_content_start_us = timestamp_us;
			}
			if (m_play_intro && intro_seconds() >= c_intro_end + 1.f)
			{
				m_play_intro = false;
			}
		}

		if (m_reload_requested.exchange(false))
		{
			m_enumeration_thread.reset();
			start_reload();
		}

		if (m_fade_animation.active)
		{
			m_fade_animation.update(timestamp_us);
		}

		// The new game's art fading in over the last one's
		{
			std::lock_guard lock(m_mutex);
			if (m_background_fading)
			{
				if (!m_background_fade_start)
				{
					m_background_fade_start = timestamp_us;
				}
				const f32 t = std::min(1.f, static_cast<f32>(timestamp_us - m_background_fade_start) / c_background_fade_us);
				m_background.fore_color.a = t * t * (3.f - 2.f * t);
				m_background.refresh();
				m_background_fading = t < 1.f;
			}
		}

		{
			// The Library's row eases toward the selection
			std::lock_guard lock(m_mutex);
			const f32 dt = m_last_update_us ? std::min(0.1f, (timestamp_us - m_last_update_us) / 1'000'000.f) : 0.f;
			m_last_update_us = timestamp_us;
			const f32 target = static_cast<f32>(m_selected);
			m_flow_pos += (target - m_flow_pos) * (1.f - std::exp(-dt * 11.f));
			if (std::abs(target - m_flow_pos) < 0.001f)
			{
				m_flow_pos = target;
			}
		}

		if (m_tab == tab::settings)
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

		// Any button during the opening ends it
		if (intro_running())
		{
			skip_intro();
			return;
		}

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

		if (m_gs_open)
		{
			handle_game_settings(button_press);
			return;
		}

		// Tabs change from anywhere at the top level
		if (button_press == pad_button::L1 || button_press == pad_button::R1)
		{
			const s32 step = button_press == pad_button::L1 ? -1 : 1;
			set_tab(static_cast<tab>(std::clamp(static_cast<s32>(m_tab) + step, 0, 2)));
			return;
		}

		if (m_tab == tab::library)
		{
			handle_library(button_press);
			return;
		}

		if (m_tab != tab::home)
		{
			home_menu_page& page = *m_settings;
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
			open_game_settings();
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
			case focus::settings: open_game_settings(); break;
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

		const f32 intro = intro_seconds();
		const f32 content = content_seconds();

		// A step of the hero or the row: rising `rise` pixels into place
		const auto step = [&](f32 start, f32 length = 0.5f) { return ease_out(progress(content, start, length)); };

		compiled_resource result;
		result.add(m_backdrop.get_compiled());
		if (m_tab != tab::library || m_gs_open)
		{
			compiled_resource art;
			if (m_background_fading)
			{
				art.add(m_background_prev.get_compiled());
			}
			if (m_background_image)
			{
				art.add(m_background.get_compiled());
			}
			add_animated(result, art, step(0.f, 0.7f));
		}
		result.add(m_wash.get_compiled());
		result.add(m_fade_left.get_compiled());
		result.add(m_fade_top.get_compiled());
		result.add(m_fade_bottom.get_compiled());

		if (m_gs_open)
		{
			const f32 page = ease_out(progress((m_now_us - m_gs_open_us) / 1'000'000.f, 0.f, 0.3f));
			compiled_resource items;
			for (const auto& item : m_gs_items)
			{
				items.add(item->get_compiled());
			}
			add_animated(result, items, page, 0.f, 14.f * (1.f - page));
		}
		else if (m_tab == tab::home)
		{
			const f32 welcome = step(0.05f);
			const f32 title = step(0.12f);
			const f32 chips = step(0.2f);
			const f32 buttons = step(0.28f);
			add_animated(result, m_welcome.get_compiled(), welcome, 0.f, 18.f * (1.f - welcome));
			add_animated(result, m_title.get_compiled(), title, 0.f, 18.f * (1.f - title));
			for (usz i = 0; i < m_chips.size(); i++)
			{
				compiled_resource chip;
				chip.add(m_chips[i]->get_compiled());
				chip.add(m_chip_labels[i]->get_compiled());
				add_animated(result, chip, chips, 0.f, 18.f * (1.f - chips));
			}

			if (!m_games.empty())
			{
				compiled_resource row;
				for (overlay_element* element : std::initializer_list<overlay_element*>{&m_play_button, &m_play_icon, &m_play_label, &m_settings_button,
						&m_settings_icon, &m_settings_label, &m_delete_button, &m_delete_icon, &m_delete_label})
				{
					row.add(element->get_compiled());
				}
				add_animated(result, row, buttons, 0.f, 18.f * (1.f - buttons));
			}

			{
				const f32 header = step(0.3f);
				compiled_resource heading;
				heading.add(m_row_title.get_compiled());
				heading.add(m_row_rule.get_compiled());
				add_animated(result, heading, header);
			}

			if (m_games.empty())
			{
				add_animated(result, m_placeholder.get_compiled(), ease_out(progress(intro, c_intro_bar, 0.5f)));
			}
			else
			{
				// The tiles one after another, left to right
				for (usz i = 0; i < m_tiles.size(); i++)
				{
					const f32 tile = step(0.36f + 0.06f * i, 0.55f);
					compiled_resource part;
					if (m_first_visible + static_cast<s32>(i) == m_selected)
					{
						part.add(m_highlight.get_compiled());
					}
					part.add(m_tiles[i]->get_compiled());
					part.add(m_tile_labels[i]->get_compiled());
					add_animated(result, part, tile, 0.f, 26.f * (1.f - tile));
				}
			}
		}
		else if (m_tab == tab::library)
		{
			compile_library(result);
		}
		else
		{
			// A darker wash under the stock pages, for their text
			overlay_element dim;
			dim.set_size(virtual_width, virtual_height);
			dim.back_color = color4f(0.f, 0.f, 0.f, 0.7f);
			result.add(dim.get_compiled());
			result.add(m_settings->get_compiled());
		}

		// The top bar comes down after the logo has arrived
		{
			const f32 bar = ease_out(progress(intro, c_intro_bar, 0.5f));
			compiled_resource top;
			top.add(m_bar_divider.get_compiled());
			for (const auto& tab_label : m_tab_labels)
			{
				top.add(tab_label->get_compiled());
			}
			top.add(m_tab_underline.get_compiled());
			top.add(m_avatar.get_compiled());
			top.add(m_avatar_letter.get_compiled());
			top.add(m_user_name.get_compiled());
			add_animated(result, top, bar, 0.f, -10.f * (1.f - bar));

			compiled_resource hints;
			for (const auto& entry : m_hints)
			{
				hints.add(entry->icon.get_compiled());
				hints.add(entry->text.get_compiled());
			}
			add_animated(result, hints, std::min(bar, step(0.6f)));
		}

		// The splash: its backdrop over everything until it makes way, and the
		// logo, which ends as the top bar's own
		if (intro < c_intro_move + c_intro_move_length + 0.1f)
		{
			overlay_element cover;
			cover.set_size(virtual_width, virtual_height);
			cover.back_color = c_backdrop;
			add_animated(result, cover.get_compiled(), 1.f - ease_in_out(progress(intro, c_intro_reveal, 0.6f)));

			const f32 line = ease_out(progress(intro, c_intro_line, 0.75f));
			const f32 line_alpha = 1.f - progress(intro, c_intro_move - 0.1f, 0.25f);
			const f32 move = ease_in_out(progress(intro, c_intro_move, c_intro_move_length));
			const f32 appear = ease_out(progress(intro, c_intro_logo_in, 0.5f));

			if (m_logo_data)
			{
				// From 2.5 times its size in the middle (growing a little as it
				// fades in) to its place in the bar
				const f32 big = 2.4f + 0.1f * appear;
				const f32 scale = big + (1.f - big) * move;
				const f32 w = m_logo.w * scale;
				const f32 h = m_logo.h * scale;
				const f32 from_x = (virtual_width - m_logo.w * big) / 2.f;
				const f32 from_y = (virtual_height - m_logo.h * big) / 2.f - 20.f;
				image_view logo;
				logo.set_raw_image(m_logo_data.get());
				logo.back_color.a = 0.f;
				logo.set_pos(static_cast<s16>(std::lround(from_x + (m_logo.x - from_x) * move)), static_cast<s16>(std::lround(from_y + (m_logo.y - from_y) * move)));
				logo.set_size(static_cast<u16>(std::lround(w)), static_cast<u16>(std::lround(h)));
				add_animated(result, logo.get_compiled(), appear);

				rounded_rect track;
				track.border_radius = 2;
				track.back_color = color4f(1.f, 1.f, 1.f, 0.12f);
				track.set_size(200, 3);
				track.set_pos(static_cast<s16>((virtual_width - 200) / 2), static_cast<s16>(from_y + m_logo.h * big + 34));
				add_animated(result, track.get_compiled(), line_alpha * appear);

				rounded_rect fill;
				fill.border_radius = 2;
				fill.back_color = c_accent;
				fill.set_size(static_cast<u16>(std::max(4.f, 200.f * line)), 3);
				fill.set_pos(track.x, track.y);
				add_animated(result, fill.get_compiled(), line_alpha * appear);
			}
		}
		else
		{
			result.add(m_logo_data ? m_logo.get_compiled() : m_logo_text.get_compiled());
		}

		if (m_confirm_delete || m_deleting || !m_delete_result.empty())
		{
			result.add(m_confirm_dim.get_compiled());
			result.add(m_confirm_panel.get_compiled());
			result.add(m_confirm_title.get_compiled());
			result.add(m_confirm_body.get_compiled());
			if (!m_deleting)
			{
				for (hint* entry : {&m_confirm_yes, &m_confirm_no})
				{
					result.add(entry->icon.get_compiled());
					result.add(entry->text.get_compiled());
				}
			}
		}

		m_fade_animation.apply(result);
		return result;
	}

	void ps5_launcher_dialog::show()
	{
		// The first opening has its splash instead of the quick fade
		m_fade_animation.current = color4f(0.f);
		m_fade_animation.end = color4f(1.f);
		m_fade_animation.active = !m_play_intro;

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
