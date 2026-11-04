#pragma once
#include "config.h"
#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct EncodedAudioPacket {
    std::vector<uint8_t> data; // Raw AAC frame (no ADTS header)
    uint64_t timestamp_us = 0;
    uint64_t duration_us = 0;  // 1024 samples @ 48kHz = 21333 us
};

class AudioCapture {
public:
    using PacketCallback = std::function<void(const EncodedAudioPacket&)>;

    AudioCapture() = default;
    ~AudioCapture();

    bool Start(AudioMode mode, int bitrateKbps, PacketCallback onPacket);
    void Stop();

    int GetSampleRate() const { return m_sampleRate; }
    int GetChannels() const { return m_channels; }
    const std::vector<uint8_t>& GetAudioSpecificConfig() const { return m_asc; }

private:
    void ThreadMain();
    bool InitAacEncoder();
    bool InitProcessLoopback(DWORD targetPid, ComPtr<IAudioClient>& outClient, WAVEFORMATEX** outWfx);
    bool InitDesktopLoopback(ComPtr<IAudioClient>& outClient, WAVEFORMATEX** outWfx);
    void FeedPcmToAac(const int16_t* samples, size_t frameCount, uint64_t baseTimestampUs);

    AudioMode m_mode = AudioMode::ActiveWindow;
    int m_bitrateKbps = 160;
    PacketCallback m_onPacket;

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    int m_sampleRate = 48000;
    int m_channels = 2;
    std::vector<uint8_t> m_asc = { 0x11, 0x90 }; // Default AAC-LC 48kHz stereo AudioSpecificConfig

    ComPtr<IMFTransform> m_aacEncoder;
    std::vector<int16_t> m_pcmBuffer;
    uint64_t m_nextAacPtsUs = 0;
};
