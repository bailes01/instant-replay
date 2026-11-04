#pragma once
#include "nvenc_encoder.h"
#include "audio_capture.h"
#include <deque>
#include <mutex>
#include <vector>

struct ReplaySnapshot {
    std::vector<EncodedVideoPacket> video_packets;
    std::vector<EncodedAudioPacket> audio_packets;
};

class ReplayBuffer {
public:
    explicit ReplayBuffer(int maxSeconds = 30);

    void SetMaxSeconds(int maxSeconds);
    void PushVideo(EncodedVideoPacket&& pkt);
    void PushAudio(const EncodedAudioPacket& pkt);
    void Clear();

    size_t GetMemoryBytes() const;
    size_t GetVideoFrameCount() const;
    ReplaySnapshot Snapshot() const;

private:
    void PruneLocked(uint64_t latestTimestampUs);

    mutable std::mutex m_mutex;
    uint64_t m_maxDurationUs = 30ULL * 1000000ULL;
    std::deque<EncodedVideoPacket> m_video;
    std::deque<EncodedAudioPacket> m_audio;
};
