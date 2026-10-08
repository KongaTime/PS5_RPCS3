#pragma once

// PS5: frame generation. Before each of the game's frames the screen shows
// one made between it and the frame before, when the game runs slower than
// the display can show twice (VKPresent.cpp): about twice as many frames
// reach the screen, the game's own each one refresh later.
//
// Motion is estimated from the pictures alone (a PS3 game gives no motion
// vectors): both frames' brightness at a working size of at most 640 across,
// blocks matched at a quarter of that and refined at full working size, the
// vectors' outliers removed by a median; then each pixel of the frame between
// is taken half way along its vector from both frames. Where a block matched
// badly, or the two frames disagree there, it leans on the newer frame, so a
// wrong vector costs smoothness rather than smearing the picture.

#include "VKCompute.h"
#include "vkutils/image.h"
#include "vkutils/sampler.h"

#include <memory>

namespace vk
{
	class frame_generator
	{
	public:
		~frame_generator();

		// Records the work for the game's new frame `cur` (its first width x
		// height texels): with `generate`, the frame between it and the last
		// one, which it returns; and in any case what the next frame needs of
		// this one. Returns nullptr when there is no frame between (the first
		// frame, a new size, or none asked for)
		vk::viewable_image* process(const vk::command_buffer& cmd, vk::viewable_image* cur, u32 width, u32 height, bool generate);

		// Forgets the last frame: the next one is a first one
		void reset();

	private:
		void allocate(u32 width, u32 height);
		void dispose();

		u32 m_width = 0;
		u32 m_height = 0;
		u32 m_work_w = 0;   // the brightness's working size
		u32 m_work_h = 0;
		bool m_have_prev = false;

		std::unique_ptr<vk::viewable_image> m_prev_color;        // the last frame, at its own size
		std::unique_ptr<vk::viewable_image> m_luma[2];           // brightness at the working size: [0] the last frame's, [1] this one's
		std::unique_ptr<vk::viewable_image> m_luma_coarse[2];    // and at a quarter of it
		std::unique_ptr<vk::viewable_image> m_vectors_coarse;    // a vector per 4x4 block at the coarse size
		std::unique_ptr<vk::viewable_image> m_vectors;           // a vector per 8x8 block at the working size
		std::unique_ptr<vk::viewable_image> m_vectors_smooth;    // the same, outliers removed
		std::unique_ptr<vk::viewable_image> m_output;            // the frame between
	};
}
