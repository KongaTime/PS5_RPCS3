// PS5: a sound file played once, on an audio port of its own (the launcher's
// intro). RPCS3's own overlay sounds go through Emu callbacks the PS5
// frontend leaves silent; this plays the title's own WAV files.
//
// libSceAudioOut as ps5_audio_backend.cpp drives it: a main port for the
// system user, 256-frame grains at 48 kHz in signed 16-bit stereo,
// sceAudioOutOutput blocking for one grain. The console mixes the port with
// RPCS3's own.

#include "stdafx.h"
#include "Utilities/File.h"

#include <cstring>
#include <thread>

LOG_CHANNEL(ps5_sound, "PS5Sound");

extern "C"
{
	int sceAudioOutInit(void);
	int sceAudioOutOpen(s32 user, s32 type, s32 index, u32 grain, u32 rate, u32 format);
	int sceAudioOutOutput(s32 port, const void* samples);
	int sceAudioOutClose(s32 port);
}

namespace
{
	constexpr u32 c_grain = 256;
	constexpr u32 c_rate = 48000;
	constexpr s32 c_system_user = 0xff;
	constexpr s32 c_port_main = 0;
	constexpr u32 c_format_s16_stereo = 1;
	constexpr int c_already_initialised = static_cast<int>(0x8026000e);

	// Raised by ps5_stop_sounds; each playing sound fades out over a few grains
	atomic_t<u32> g_stop_generation = 0;

	// The samples of a 48 kHz, 16-bit PCM WAV, as stereo frames
	bool read_wav(const std::string& path, std::vector<s16>& frames)
	{
		fs::file file(path);
		if (!file)
		{
			ps5_sound.error("%s: not found", path);
			return false;
		}
		const std::vector<u8> data = file.to_vector<u8>();
		if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) || std::memcmp(data.data() + 8, "WAVE", 4))
		{
			ps5_sound.error("%s: not a WAV file", path);
			return false;
		}

		u16 format = 0, channels = 0, bits = 0;
		u32 rate = 0;
		for (usz at = 12; at + 8 <= data.size();)
		{
			u32 size;
			std::memcpy(&size, data.data() + at + 4, 4);
			const u8* body = data.data() + at + 8;
			if (at + 8 + size > data.size())
			{
				break;
			}
			if (!std::memcmp(data.data() + at, "fmt ", 4) && size >= 16)
			{
				std::memcpy(&format, body, 2);
				std::memcpy(&channels, body + 2, 2);
				std::memcpy(&rate, body + 4, 4);
				std::memcpy(&bits, body + 14, 2);
			}
			else if (!std::memcmp(data.data() + at, "data", 4))
			{
				if (format != 1 || bits != 16 || rate != c_rate || (channels != 1 && channels != 2))
				{
					ps5_sound.error("%s: wants 48 kHz 16-bit PCM, mono or stereo (format %u, %u channels, %u Hz, %u bits)", path, format, channels, rate, bits);
					return false;
				}
				const usz count = size / (2 * channels);
				frames.resize(count * 2);
				for (usz i = 0; i < count; i++)
				{
					s16 l, r;
					std::memcpy(&l, body + i * 2 * channels, 2);
					r = l;
					if (channels == 2)
					{
						std::memcpy(&r, body + i * 4 + 2, 2);
					}
					frames[i * 2] = l;
					frames[i * 2 + 1] = r;
				}
				return true;
			}
			at += 8 + size + (size & 1);
		}

		ps5_sound.error("%s: no PCM data", path);
		return false;
	}
}

void ps5_play_sound_file(const std::string& path)
{
	std::vector<s16> frames;
	if (!read_wav(path, frames))
	{
		return;
	}

	std::thread([frames = std::move(frames), path, generation = g_stop_generation.load()]()
	{
		if (const int init = sceAudioOutInit(); init < 0 && init != c_already_initialised)
		{
			ps5_sound.error("sceAudioOutInit failed: 0x%08x", static_cast<u32>(init));
			return;
		}
		const s32 port = sceAudioOutOpen(c_system_user, c_port_main, 0, c_grain, c_rate, c_format_s16_stereo);
		if (port < 0)
		{
			ps5_sound.error("%s: sceAudioOutOpen failed: 0x%08x", path, static_cast<u32>(port));
			return;
		}

		// Stopped: fade over these grains, then end
		constexpr u32 fade_grains = 8;
		u32 fading = 0;
		alignas(64) s16 grain[c_grain * 2];
		const usz total = frames.size() / 2;
		for (usz at = 0; at < total && fading <= fade_grains; at += c_grain)
		{
			if (!fading && g_stop_generation != generation)
			{
				fading = 1;
			}
			const usz count = std::min<usz>(c_grain, total - at);
			std::memcpy(grain, frames.data() + at * 2, count * 4);
			std::memset(grain + count * 2, 0, (c_grain - count) * 4);
			if (fading)
			{
				const f32 from = 1.f - static_cast<f32>(fading - 1) / fade_grains;
				const f32 to = 1.f - static_cast<f32>(fading) / fade_grains;
				for (u32 i = 0; i < c_grain; i++)
				{
					const f32 gain = from + (to - from) * i / c_grain;
					grain[i * 2] = static_cast<s16>(grain[i * 2] * gain);
					grain[i * 2 + 1] = static_cast<s16>(grain[i * 2 + 1] * gain);
				}
				fading++;
			}
			if (sceAudioOutOutput(port, grain) < 0)
			{
				ps5_sound.error("%s: sceAudioOutOutput failed", path);
				break;
			}
		}

		// The grain still queued
		sceAudioOutOutput(port, nullptr);
		sceAudioOutClose(port);
	}).detach();
}

void ps5_stop_sounds()
{
	g_stop_generation++;
}
