#pragma once
#include "config.h"
#include <d3d11.h>
#include <cstdint>
#include <vector>
#include <string>
#include <ffnvcodec/nvEncodeAPI.h>

struct EncodedVideoPacket {
    std::vector<uint8_t> data;
    uint64_t timestamp_us = 0;
    uint64_t duration_us = 0;
    bool is_keyframe = false;
};

class NvencEncoder {
public:
    NvencEncoder() = default;
    ~NvencEncoder();

    bool Init(ID3D11Device* device, ID3D11Texture2D* sharedInputTex,
              int width, int height, int fps, VideoCodec codec,
              int bitrateKbps, int preset);
    void Shutdown();

    bool EncodeFrame(uint64_t timestampUs, uint64_t durationUs, bool forceIdr, EncodedVideoPacket& outPkt);

    VideoCodec GetCodec() const { return m_codec; }
    int GetBitrateKbps() const { return m_bitrateKbps; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    int GetFps() const { return m_fps; }
    const std::vector<uint8_t>& GetSeqParams() const { return m_seqParams; }

private:
    HMODULE m_nvencLib = nullptr;
    NV_ENCODE_API_FUNCTION_LIST m_fn{};
    void* m_encoder = nullptr;
    NV_ENC_REGISTERED_PTR m_registeredRes = nullptr;
    NV_ENC_OUTPUT_PTR m_bitstreamBuf = nullptr;

    VideoCodec m_codec = VideoCodec::AV1;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 60;
    int m_bitrateKbps = 16000;
    std::vector<uint8_t> m_seqParams;
};
