#include "replay_buffer.h"

ReplayBuffer::ReplayBuffer(int maxSeconds)
    : m_maxDurationUs(static_cast<uint64_t>(maxSeconds) * 1000000ULL) {}

void ReplayBuffer::SetMaxSeconds(int maxSeconds) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_maxDurationUs = static_cast<uint64_t>(maxSeconds) * 1000000ULL;
}

void ReplayBuffer::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_video.clear();
    m_audio.clear();
}

size_t ReplayBuffer::GetMemoryBytes() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    size_t total = 0;
    for (const auto& v : m_video) total += v.data.capacity() + sizeof(EncodedVideoPacket);
    for (const auto& a : m_audio) total += a.data.capacity() + sizeof(EncodedAudioPacket);
    return total;
}

size_t ReplayBuffer::GetVideoFrameCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_video.size();
}

void ReplayBuffer::PushVideo(EncodedVideoPacket&& pkt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint64_t ts = pkt.timestamp_us;
    m_video.push_back(std::move(pkt));
    PruneLocked(ts);
}

void ReplayBuffer::PushAudio(const EncodedAudioPacket& pkt) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_audio.push_back(pkt);
    if (!m_audio.empty()) {
        uint64_t latest = m_audio.back().timestamp_us;
        while (m_audio.size() > 1 && latest > m_audio.front().timestamp_us + m_maxDurationUs + 2000000ULL) {
            m_audio.pop_front();
        }
    }
}

void ReplayBuffer::PruneLocked(uint64_t latestTimestampUs) {
    // Keep at most m_maxDurationUs back to the nearest preceding keyframe
    if (m_video.size() <= 2) return;

    size_t lastUsefulKeyframeIdx = 0;
    for (size_t i = 0; i < m_video.size(); ++i) {
        if (m_video[i].is_keyframe) {
            if (latestTimestampUs >= m_video[i].timestamp_us &&
                (latestTimestampUs - m_video[i].timestamp_us) >= m_maxDurationUs) {
                lastUsefulKeyframeIdx = i;
            }
        }
    }

    if (lastUsefulKeyframeIdx > 0) {
        m_video.erase(m_video.begin(), m_video.begin() + lastUsefulKeyframeIdx);
    }

    if (!m_video.empty()) {
        uint64_t oldestVideoTs = m_video.front().timestamp_us;
        while (!m_audio.empty() && m_audio.front().timestamp_us + m_audio.front().duration_us < oldestVideoTs) {
            m_audio.pop_front();
        }
    }
}

ReplaySnapshot ReplayBuffer::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ReplaySnapshot snap;
    if (m_video.empty()) return snap;

    uint64_t latestTs = m_video.back().timestamp_us;
    size_t startIdx = 0;
    bool foundKeyframe = false;

    // Walk backwards or forwards to find the first keyframe within maxDurationUs
    for (size_t i = 0; i < m_video.size(); ++i) {
        if (m_video[i].is_keyframe) {
            if (!foundKeyframe) {
                startIdx = i;
                foundKeyframe = true;
            }
            if (latestTs >= m_video[i].timestamp_us &&
                (latestTs - m_video[i].timestamp_us) <= m_maxDurationUs + 500000ULL) {
                startIdx = i;
                break;
            }
            startIdx = i;
        }
    }

    if (!foundKeyframe) return snap;

    snap.video_packets.assign(m_video.begin() + startIdx, m_video.end());
    uint64_t startTs = snap.video_packets.front().timestamp_us;
    uint64_t endTs = snap.video_packets.back().timestamp_us + snap.video_packets.back().duration_us;

    for (const auto& a : m_audio) {
        if (a.timestamp_us >= startTs && a.timestamp_us <= endTs) {
            snap.audio_packets.push_back(a);
        }
    }

    return snap;
}
