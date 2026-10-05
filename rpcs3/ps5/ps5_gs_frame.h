#pragma once

// PS5: the frame RPCS3's renderer draws into. The console has no windows; the
// Vulkan renderer's surface is the whole display (vkutils/swapchain_ps5.hpp),
// so this frame only reports the display's size and rate and keeps state.

#include "Emu/RSX/GSFrameBase.h"

class ps5_gs_frame final : public GSFrameBase
{
public:
	ps5_gs_frame(int width, int height, f64 rate);

	void close() override;
	void reset() override;
	bool shown() override;
	void hide() override;
	void show() override;
	void toggle_fullscreen() override;

	void delete_context(draw_context_t ctx) override;
	draw_context_t make_context() override;
	void set_current(draw_context_t ctx) override;
	void flip(draw_context_t ctx, bool skip_frame = false) override;
	int client_width() override;
	int client_height() override;
	f64 client_display_rate() override;
	bool has_alpha() override;

	display_handle_t handle() const override;

	bool can_consume_frame() const override;
	void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override;
	void take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra) override;

	void update_title(double fps = 0.0) override;

private:
	int m_width;
	int m_height;
	f64 m_rate;
	bool m_shown = false;
};
