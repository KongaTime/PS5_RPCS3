#pragma once

// PS5: RPCS3's audio on the console's own output (libSceAudioOut).
//
// Each backend RPCS3 opens (cellAudio's, RSXAudio's) gets a main port of its
// own and a worker thread that pulls one 256-frame grain from RPCS3's write
// callback, converts it to 48 kHz signed 16-bit stereo, and blocks in
// sceAudioOutOutput until the console takes it: the output's own pace is the
// clock, as on the desktop's cubeb backend.

#include "Emu/Audio/AudioBackend.h"

#include <atomic>
#include <thread>
#include <vector>

class ps5_audio_backend final : public AudioBackend
{
public:
	ps5_audio_backend() = default;
	~ps5_audio_backend() override;

	ps5_audio_backend(const ps5_audio_backend&) = delete;
	ps5_audio_backend& operator=(const ps5_audio_backend&) = delete;

	std::string_view GetName() const override { return "PS5"sv; }

	bool Open(std::string_view dev_id, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override;
	void Close() override;

	f64 GetCallbackFrameLen() override;

	void Play() override;
	void Pause() override;

private:
	void output_loop();

	// Fills out (grain frames of interleaved stereo S16) from the write
	// callback, or with silence; says how much the callback had
	enum class fill_result { full, short_read, empty };
	fill_result fill(s16* out);

	s32 m_port = -1;
	std::thread m_thread;
	std::atomic<bool> m_running = false;

	// The write callback's samples at the stream's rate, and where the
	// resampler is between the last two frames read (rates other than 48 kHz)
	std::vector<u8> m_in;
	f32 m_last[2]{};
	f64 m_phase = 0.0;
	u32 m_errors = 0;
};
