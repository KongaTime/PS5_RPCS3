#pragma once

#include "Emu/Audio/AudioBackend.h"

class NullAudioBackend final : public AudioBackend
{
public:
	NullAudioBackend() {}
	~NullAudioBackend() {}

	std::string_view GetName() const override { return "Null"sv; }

	bool Open(std::string_view /* dev_id */, AudioFreq freq, AudioSampleSize sample_size, AudioChannelCnt ch_cnt, audio_channel_layout layout) override
	{
		Close();
#ifdef __PROSPERO__
		// PS5: the format as asked, as every other backend sets it: left as
		// constructed, the layout stayed automatic (0), and cellAudio's thread
		// died on "Unsupported layout 0" (the Ratchet & Clank Collection, on
		// my console)
		m_sampling_rate = freq;
		m_sample_size = sample_size;
		const u32 channels = static_cast<u32>(ch_cnt);
		m_layout = layout == audio_channel_layout::automatic ? default_layout(channels) : layout;
		m_channels = layout_channel_count(channels, m_layout);
#else
		static_cast<void>(freq);
		static_cast<void>(sample_size);
		static_cast<void>(ch_cnt);
		static_cast<void>(layout);
#endif
		return true;
	}
	void Close() override { m_playing = false; }

	f64 GetCallbackFrameLen() override { return 0.01; };

	void Play() override { m_playing = true; }
	void Pause() override { m_playing = false; }
	bool IsPlaying() override { return m_playing; }

private:
	bool m_playing = false;
};
