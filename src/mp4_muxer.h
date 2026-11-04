#pragma once
#include "config.h"
#include "replay_buffer.h"
#include <string>
#include <vector>
#include <cstdint>

class Mp4Muxer {
public:
    static bool WriteClip(
        const std::string& filePath,
        VideoCodec codec,
        int width,
        int height,
        int fps,
        const std::vector<uint8_t>& videoSeqParams,
        const std::vector<uint8_t>& audioSpecificConfig,
        int audioSampleRate,
        int audioChannels,
        const ReplaySnapshot& snapshot
    );
};
