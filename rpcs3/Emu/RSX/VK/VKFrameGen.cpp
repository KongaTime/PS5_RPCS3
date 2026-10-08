#include "stdafx.h"
#include "VKFrameGen.h"

#include "VKHelpers.h"
#include "VKResourceManager.h"
#include "vkutils/barriers.h"
#include "vkutils/device.h"

#include "util/asm.hpp"

// PS5: the frames made since the title started, for the trace's status line
atomic_t<u64> g_ps5_frames_generated = 0;

namespace vk
{
	namespace
	{
		// Every pass: 8x8 invocations a group, three inputs sampled (unused ones
		// bound to the first), one output written, eight numbers pushed
		constexpr const char* c_header = R"(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2) uniform sampler2D tex2;
layout(set = 0, binding = 3, %FORMAT%) uniform writeonly image2D dst;
layout(push_constant) uniform push_block { vec4 p0; vec4 p1; };
)";

		enum pass_kind : int
		{
			pass_brightness,     // the frame's brightness at the working size
			pass_quarter,        // that at a quarter of it, 4x4 averaged
			pass_coarse_search,  // a vector per 4x4 block of the quarter size
			pass_fine_search,    // a vector per 8x8 block of the working size, from the coarse ones
			pass_median,         // the vectors' 3x3 median
			pass_between,        // the frame between
			pass_keep,           // the frame, kept for the next one
		};

		constexpr const char* c_formats[] = { "r32f", "r32f", "rgba16f", "rgba16f", "rgba16f", "rgba8", "rgba8" };

		constexpr const char* c_bodies[] =
		{
			// pass_brightness: tex0 the frame; p0 = (its used part as a share of
			// its texture across and down, the working size)
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	const vec2 size = p0.zw;
	if (p.x >= int(size.x) || p.y >= int(size.y)) return;
	float l = 0.0;
	for (int j = 0; j < 2; j++)
	{
		for (int i = 0; i < 2; i++)
		{
			const vec2 uv = (vec2(p) + vec2(0.25 + 0.5 * float(i), 0.25 + 0.5 * float(j))) / size * p0.xy;
			l += dot(texture(tex0, uv).rgb, vec3(0.299, 0.587, 0.114));
		}
	}
	imageStore(dst, p, vec4(l * 0.25));
}
)",
			// pass_quarter: tex0 the working brightness; p0 = (the working size, the quarter size)
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.z) || p.y >= int(p0.w)) return;
	const ivec2 lim = ivec2(p0.xy) - 1;
	float l = 0.0;
	for (int j = 0; j < 4; j++)
	{
		for (int i = 0; i < 4; i++)
		{
			l += texelFetch(tex0, min(p * 4 + ivec2(i, j), lim), 0).r;
		}
	}
	imageStore(dst, p, vec4(l / 16.0));
}
)",
			// pass_coarse_search: tex0 the last frame's quarter brightness, tex1
			// this one's; p0 = (the quarter size, its 4x4 block grid). Each block
			// of this frame is sought in the last one up to 12 texels away (48 at
			// the working size: a quick camera turn at 30 fps; at 6, synthetic
			// pans of 40 lost to a plain blend); the vector points from here to
			// there, its z the mean difference per texel, a little in favour of
			// short vectors
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.z) || p.y >= int(p0.w)) return;
	const ivec2 lim = ivec2(p0.xy) - 1;
	const ivec2 base = p * 4;
	float cur[16];
	for (int k = 0; k < 16; k++)
	{
		cur[k] = texelFetch(tex1, min(base + ivec2(k & 3, k >> 2), lim), 0).r;
	}
	float best = 1e9;
	vec2 best_v = vec2(0.0);
	for (int dy = -12; dy <= 12; dy++)
	{
		for (int dx = -12; dx <= 12; dx++)
		{
			float s = 0.0;
			for (int k = 0; k < 16; k++)
			{
				s += abs(cur[k] - texelFetch(tex0, clamp(base + ivec2(k & 3, k >> 2) + ivec2(dx, dy), ivec2(0), lim), 0).r);
			}
			s = s / 16.0 + 0.0015 * float(abs(dx) + abs(dy));
			if (s < best)
			{
				best = s;
				best_v = vec2(dx, dy);
			}
		}
	}
	imageStore(dst, p, vec4(best_v, best, 0.0));
}
)",
			// pass_fine_search: tex0 the last frame's working brightness, tex1
			// this one's, tex2 the coarse vectors; p0 = (the working size, its
			// 8x8 block grid), p1.xy the coarse grid. Each block starts from no
			// motion and from the coarse vectors of its coarse block and its four
			// neighbours (four times as long here), and looks 2 texels around each
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.z) || p.y >= int(p0.w)) return;
	const ivec2 lim = ivec2(p0.xy) - 1;
	const ivec2 coarse_lim = ivec2(p1.xy) - 1;
	const ivec2 base = p * 8;
	float cur[64];
	for (int k = 0; k < 64; k++)
	{
		cur[k] = texelFetch(tex1, min(base + ivec2(k & 7, k >> 3), lim), 0).r;
	}
	const ivec2 cb = p / 2;
	ivec2 starts[6];
	starts[0] = ivec2(0);
	starts[1] = ivec2(round(texelFetch(tex2, clamp(cb, ivec2(0), coarse_lim), 0).xy * 4.0));
	starts[2] = ivec2(round(texelFetch(tex2, clamp(cb + ivec2(1, 0), ivec2(0), coarse_lim), 0).xy * 4.0));
	starts[3] = ivec2(round(texelFetch(tex2, clamp(cb - ivec2(1, 0), ivec2(0), coarse_lim), 0).xy * 4.0));
	starts[4] = ivec2(round(texelFetch(tex2, clamp(cb + ivec2(0, 1), ivec2(0), coarse_lim), 0).xy * 4.0));
	starts[5] = ivec2(round(texelFetch(tex2, clamp(cb - ivec2(0, 1), ivec2(0), coarse_lim), 0).xy * 4.0));
	float best = 1e9;
	vec2 best_v = vec2(0.0);
	for (int c = 0; c < 6; c++)
	{
		for (int dy = -2; dy <= 2; dy++)
		{
			for (int dx = -2; dx <= 2; dx++)
			{
				const ivec2 v = starts[c] + ivec2(dx, dy);
				float s = 0.0;
				for (int k = 0; k < 64; k++)
				{
					s += abs(cur[k] - texelFetch(tex0, clamp(base + ivec2(k & 7, k >> 3) + v, ivec2(0), lim), 0).r);
				}
				s = s / 64.0 + 0.0008 * float(abs(v.x) + abs(v.y));
				if (s < best)
				{
					best = s;
					best_v = vec2(v);
				}
			}
		}
	}
	imageStore(dst, p, vec4(best_v, best, 0.0));
}
)",
			// pass_median: tex0 the vectors; p0.xy their grid. Each vector's
			// parts become the medians of its 3x3 neighbourhood's, which drops
			// lone wrong ones; the match error stays its own
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.x) || p.y >= int(p0.y)) return;
	const ivec2 lim = ivec2(p0.xy) - 1;
	float xs[9];
	float ys[9];
	int n = 0;
	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			const vec4 v = texelFetch(tex0, clamp(p + ivec2(i, j), ivec2(0), lim), 0);
			xs[n] = v.x;
			ys[n] = v.y;
			n++;
		}
	}
	for (int a = 1; a < 9; a++)
	{
		const float x = xs[a];
		const float y = ys[a];
		int b = a - 1;
		while (b >= 0 && xs[b] > x)
		{
			xs[b + 1] = xs[b];
			b--;
		}
		xs[b + 1] = x;
		b = a - 1;
		while (b >= 0 && ys[b] > y)
		{
			ys[b + 1] = ys[b];
			b--;
		}
		ys[b + 1] = y;
	}
	imageStore(dst, p, vec4(xs[4], ys[4], texelFetch(tex0, p, 0).z, 0.0));
}
)",
			// pass_between: tex0 the last frame (its own size), tex1 this one,
			// tex2 the vectors; p0 = (this frame's used part as a share of its
			// texture, the output size), p1 = (the working size, the share of
			// the vector grid it covers). Half way along each vector in both
			// frames, averaged where they agree; where the match was poor, this
			// frame as it is
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.z) || p.y >= int(p0.w)) return;
	const vec2 uv = (vec2(p) + 0.5) / p0.zw;
	const vec4 mv = texture(tex2, uv * p1.zw);
	const vec2 d = mv.xy / p1.xy;
	const vec3 last = texture(tex0, uv + 0.5 * d).rgb;
	const vec3 next = texture(tex1, (uv - 0.5 * d) * p0.xy).rgb;
	const vec3 here = texture(tex1, uv * p0.xy).rgb;
	const float trust = 1.0 - smoothstep(0.035, 0.11, mv.z);
	const vec3 gap = abs(last - next);
	const float agree = 1.0 - smoothstep(0.08, 0.3, max(max(gap.r, gap.g), gap.b));
	const vec3 between = mix(next, 0.5 * (last + next), agree);
	imageStore(dst, p, vec4(mix(here, between, trust), 1.0));
}
)",
			// pass_keep: tex0 the frame; p0 = (its used part as a share of its
			// texture, the output size). Sampled, not blitted: a game's display
			// buffer may be in a format a blit cannot read
			R"(
void main()
{
	const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (p.x >= int(p0.z) || p.y >= int(p0.w)) return;
	imageStore(dst, p, vec4(texture(tex0, (vec2(p) + 0.5) / p0.zw * p0.xy).rgb, 1.0));
}
)",
		};

		template <int Kind>
		struct frame_generation_pass : compute_task
		{
			const vk::image_view* m_inputs[3]{};
			const vk::image_view* m_output = nullptr;
			std::unique_ptr<vk::sampler> m_sampler;

			frame_generation_pass()
			{
				m_src = std::string(c_header) + c_bodies[Kind];
				const usz at = m_src.find("%FORMAT%");
				m_src.replace(at, 8, c_formats[Kind]);

				ssbo_count = 0;
				use_push_constants = true;
				push_constants_size = 32;
				create();
			}

			std::vector<glsl::program_input> get_inputs() override
			{
				auto result = compute_task::get_inputs();
				for (u32 i = 0; i < 3; i++)
				{
					result.push_back(glsl::program_input::make(::glsl::program_domain::glsl_compute_program, "tex" + std::to_string(i), vk::glsl::input_type_texture, 0, i));
				}
				result.push_back(glsl::program_input::make(::glsl::program_domain::glsl_compute_program, "dst", vk::glsl::input_type_storage_texture, 0, 3));
				return result;
			}

			void bind_resources(const vk::command_buffer&) override
			{
				if (!m_sampler)
				{
					const auto pdev = vk::get_current_renderer();
					m_sampler = std::make_unique<vk::sampler>(*pdev,
						VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
						VK_FALSE, 0.f, 1.f, 0.f, 0.f, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);
				}

				for (u32 i = 0; i < 3; i++)
				{
					m_program->bind_uniform({ *m_inputs[i], *m_sampler }, 0, i);
				}
				m_program->bind_uniform({ *m_output }, 0, 3);
			}

			void run(const vk::command_buffer& cmd, std::initializer_list<vk::viewable_image*> inputs, vk::viewable_image* output, u32 width, u32 height, const std::array<f32, 8>& params)
			{
				const auto view = [](vk::viewable_image* image) { return image->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY)); };
				u32 i = 0;
				for (vk::viewable_image* input : inputs)
				{
					m_inputs[i++] = view(input);
				}
				for (; i < 3; i++)
				{
					m_inputs[i] = m_inputs[0];
				}
				m_output = view(output);

				if (!m_program)
				{
					load_program(cmd);
				}
				vkCmdPushConstants(cmd, m_program->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, push_constants_size, params.data());
				compute_task::run(cmd, utils::aligned_div(width, 8u), utils::aligned_div(height, 8u), 1);
			}
		};

		// Each pass's writes seen by the next pass's reads and writes, and by
		// copies; and the last frame's reads of these images done before this
		// one writes them
		void barrier(const vk::command_buffer& cmd, VkPipelineStageFlags from = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)
		{
			VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, from, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
		}

		std::unique_ptr<vk::viewable_image> make_image(u32 width, u32 height, VkFormat format)
		{
			const auto pdev = vk::get_current_renderer();
			auto image = std::make_unique<vk::viewable_image>(
				*pdev, pdev->get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_TYPE_2D, format, std::max(width, 1u), std::max(height, 1u), 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
				VK_IMAGE_CREATE_ALLOW_NULL_RPCS3, VMM_ALLOCATION_POOL_SWAPCHAIN, RSX_FORMAT_CLASS_COLOR);
			if (image->value == VK_NULL_HANDLE)
			{
				return nullptr;
			}
			return image;
		}

		// The working size: at most 640 across, the frame's shape kept
		void working_size(u32 width, u32 height, u32& work_w, u32& work_h)
		{
			work_w = std::min(width, 640u);
			work_h = std::max(1u, static_cast<u32>(std::lround(static_cast<f64>(height) * work_w / width)));
		}
	}

	frame_generator::~frame_generator()
	{
		// The renderer's teardown: the device is idle, the images go at once
	}

	void frame_generator::dispose()
	{
		const auto drop = [](std::unique_ptr<vk::viewable_image>& image)
		{
			if (image)
			{
				vk::get_resource_manager()->dispose(image);
			}
		};
		drop(m_prev_color);
		for (auto& image : m_luma) drop(image);
		for (auto& image : m_luma_coarse) drop(image);
		drop(m_vectors_coarse);
		drop(m_vectors);
		drop(m_vectors_smooth);
		drop(m_output);
		m_width = m_height = 0;
		m_have_prev = false;
	}

	void frame_generator::allocate(u32 width, u32 height)
	{
		dispose();

		working_size(width, height, m_work_w, m_work_h);
		const u32 coarse_w = utils::aligned_div(m_work_w, 4u);
		const u32 coarse_h = utils::aligned_div(m_work_h, 4u);

		m_prev_color = make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
		m_output = make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
		for (u32 i = 0; i < 2; i++)
		{
			m_luma[i] = make_image(m_work_w, m_work_h, VK_FORMAT_R32_SFLOAT);
			m_luma_coarse[i] = make_image(coarse_w, coarse_h, VK_FORMAT_R32_SFLOAT);
		}
		m_vectors_coarse = make_image(utils::aligned_div(coarse_w, 4u), utils::aligned_div(coarse_h, 4u), VK_FORMAT_R16G16B16A16_SFLOAT);
		m_vectors = make_image(utils::aligned_div(m_work_w, 8u), utils::aligned_div(m_work_h, 8u), VK_FORMAT_R16G16B16A16_SFLOAT);
		m_vectors_smooth = make_image(utils::aligned_div(m_work_w, 8u), utils::aligned_div(m_work_h, 8u), VK_FORMAT_R16G16B16A16_SFLOAT);

		if (!m_prev_color || !m_output || !m_luma[0] || !m_luma[1] || !m_luma_coarse[0] || !m_luma_coarse[1] || !m_vectors_coarse || !m_vectors || !m_vectors_smooth)
		{
			rsx_log.warning("Frame generation: no memory for its images at %ux%u", width, height);
			dispose();
			return;
		}

		m_width = width;
		m_height = height;
		rsx_log.notice("Frame generation: %ux%u, motion at %ux%u", width, height, m_work_w, m_work_h);
	}

	void frame_generator::reset()
	{
		m_have_prev = false;
	}

	vk::viewable_image* frame_generator::process(const vk::command_buffer& cmd, vk::viewable_image* cur, u32 width, u32 height, bool generate)
	{
		if (!cur || !width || !height)
		{
			return nullptr;
		}
		width = std::min<u32>(width, cur->width());
		height = std::min<u32>(height, cur->height());

		if (width != m_width || height != m_height)
		{
			allocate(width, height);
			if (!m_width)
			{
				return nullptr;
			}
			for (auto* image : { m_prev_color.get(), m_output.get(), m_luma[0].get(), m_luma[1].get(), m_luma_coarse[0].get(), m_luma_coarse[1].get(),
				m_vectors_coarse.get(), m_vectors.get(), m_vectors_smooth.get() })
			{
				image->change_layout(cmd, VK_IMAGE_LAYOUT_GENERAL);
			}
		}

		const bool make = generate && m_have_prev;
		const f32 uv_x = static_cast<f32>(width) / cur->width();
		const f32 uv_y = static_cast<f32>(height) / cur->height();
		const f32 work_w = static_cast<f32>(m_work_w);
		const f32 work_h = static_cast<f32>(m_work_h);
		const u32 coarse_w = utils::aligned_div(m_work_w, 4u);
		const u32 coarse_h = utils::aligned_div(m_work_h, 4u);
		const u32 coarse_grid_w = utils::aligned_div(coarse_w, 4u);
		const u32 coarse_grid_h = utils::aligned_div(coarse_h, 4u);
		const u32 grid_w = utils::aligned_div(m_work_w, 8u);
		const u32 grid_h = utils::aligned_div(m_work_h, 8u);

		// What the last frame's presentation read of these images is done
		barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

		cur->push_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// This frame's brightness, at the working size and a quarter of it
		vk::get_compute_task<frame_generation_pass<pass_brightness>>()->run(cmd, { cur }, m_luma[1].get(), m_work_w, m_work_h,
			{ uv_x, uv_y, work_w, work_h, 0.f, 0.f, 0.f, 0.f });
		barrier(cmd);
		vk::get_compute_task<frame_generation_pass<pass_quarter>>()->run(cmd, { m_luma[1].get() }, m_luma_coarse[1].get(), coarse_w, coarse_h,
			{ work_w, work_h, static_cast<f32>(coarse_w), static_cast<f32>(coarse_h), 0.f, 0.f, 0.f, 0.f });
		barrier(cmd);

		vk::viewable_image* result = nullptr;
		if (make)
		{
			vk::get_compute_task<frame_generation_pass<pass_coarse_search>>()->run(cmd, { m_luma_coarse[0].get(), m_luma_coarse[1].get() }, m_vectors_coarse.get(), coarse_grid_w, coarse_grid_h,
				{ static_cast<f32>(coarse_w), static_cast<f32>(coarse_h), static_cast<f32>(coarse_grid_w), static_cast<f32>(coarse_grid_h), 0.f, 0.f, 0.f, 0.f });
			barrier(cmd);
			vk::get_compute_task<frame_generation_pass<pass_fine_search>>()->run(cmd, { m_luma[0].get(), m_luma[1].get(), m_vectors_coarse.get() }, m_vectors.get(), grid_w, grid_h,
				{ work_w, work_h, static_cast<f32>(grid_w), static_cast<f32>(grid_h), static_cast<f32>(coarse_grid_w), static_cast<f32>(coarse_grid_h), 0.f, 0.f });
			barrier(cmd);
			vk::get_compute_task<frame_generation_pass<pass_median>>()->run(cmd, { m_vectors.get() }, m_vectors_smooth.get(), grid_w, grid_h,
				{ static_cast<f32>(grid_w), static_cast<f32>(grid_h), 0.f, 0.f, 0.f, 0.f, 0.f, 0.f });
			barrier(cmd);
			vk::get_compute_task<frame_generation_pass<pass_between>>()->run(cmd, { m_prev_color.get(), cur, m_vectors_smooth.get() }, m_output.get(), width, height,
				{ uv_x, uv_y, static_cast<f32>(width), static_cast<f32>(height), work_w, work_h, work_w / (grid_w * 8.f), work_h / (grid_h * 8.f) });
			result = m_output.get();
			g_ps5_frames_generated++;
		}

		// This frame becomes the last one: its picture, and its brightness.
		// (The frame between has read the last picture by now)
		barrier(cmd);
		vk::get_compute_task<frame_generation_pass<pass_keep>>()->run(cmd, { cur }, m_prev_color.get(), width, height,
			{ uv_x, uv_y, static_cast<f32>(width), static_cast<f32>(height), 0.f, 0.f, 0.f, 0.f });
		cur->pop_layout(cmd);
		barrier(cmd);
		std::swap(m_luma[0], m_luma[1]);
		std::swap(m_luma_coarse[0], m_luma_coarse[1]);
		m_have_prev = true;

		if (result)
		{
			// The frame between, written, for the presentation to read
			VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
		}
		return result;
	}
}
