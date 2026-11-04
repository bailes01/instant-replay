#include "nvenc_encoder.h"
#include "logger.h"
#include <cstring>

typedef NVENCSTATUS(NVENCAPI* PNVENCODEAPICREATEINSTANCE)(NV_ENCODE_API_FUNCTION_LIST*);

static GUID GetPresetGuid(int preset) {
    switch (preset) {
        case 1: return NV_ENC_PRESET_P1_GUID;
        case 2: return NV_ENC_PRESET_P2_GUID;
        case 3: return NV_ENC_PRESET_P3_GUID;
        case 4: return NV_ENC_PRESET_P4_GUID;
        case 5: return NV_ENC_PRESET_P5_GUID;
        case 6: return NV_ENC_PRESET_P6_GUID;
        case 7: return NV_ENC_PRESET_P7_GUID;
        default: return NV_ENC_PRESET_P5_GUID;
    }
}

static GUID GetCodecGuid(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::AV1:  return NV_ENC_CODEC_AV1_GUID;
        case VideoCodec::HEVC: return NV_ENC_CODEC_HEVC_GUID;
        case VideoCodec::H264: return NV_ENC_CODEC_H264_GUID;
    }
    return NV_ENC_CODEC_AV1_GUID;
}

NvencEncoder::~NvencEncoder() {
    Shutdown();
}

void NvencEncoder::Shutdown() {
    if (m_encoder) {
        if (m_bitstreamBuf && m_fn.nvEncDestroyBitstreamBuffer) {
            m_fn.nvEncDestroyBitstreamBuffer(m_encoder, m_bitstreamBuf);
            m_bitstreamBuf = nullptr;
        }
        if (m_registeredRes && m_fn.nvEncUnregisterResource) {
            m_fn.nvEncUnregisterResource(m_encoder, m_registeredRes);
            m_registeredRes = nullptr;
        }
        if (m_fn.nvEncDestroyEncoder) {
            m_fn.nvEncDestroyEncoder(m_encoder);
        }
        m_encoder = nullptr;
    }
    if (m_nvencLib) {
        FreeLibrary(m_nvencLib);
        m_nvencLib = nullptr;
    }
}

bool NvencEncoder::Init(ID3D11Device* device, ID3D11Texture2D* sharedInputTex,
                        int width, int height, int fps, VideoCodec codec,
                        int bitrateKbps, int preset) {
    Shutdown();

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_codec = codec;
    m_bitrateKbps = bitrateKbps;

    m_nvencLib = LoadLibraryA("nvEncodeAPI64.dll");
    if (!m_nvencLib) {
        LOG_ERROR("Failed to load nvEncodeAPI64.dll. Ensure NVIDIA drivers are installed.");
        return false;
    }

    auto createInstance = reinterpret_cast<PNVENCODEAPICREATEINSTANCE>(
        GetProcAddress(m_nvencLib, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        LOG_ERROR("NvEncodeAPICreateInstance not found in nvEncodeAPI64.dll.");
        return false;
    }

    memset(&m_fn, 0, sizeof(m_fn));
    m_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = createInstance(&m_fn);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("NvEncodeAPICreateInstance failed with status %d", (int)st);
        return false;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams{};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    openParams.device = device;
    openParams.apiVersion = NVENCAPI_VERSION;

    st = m_fn.nvEncOpenEncodeSessionEx(&openParams, &m_encoder);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("nvEncOpenEncodeSessionEx failed with status %d", (int)st);
        return false;
    }

    GUID encodeGuid = GetCodecGuid(codec);
    GUID presetGuid = GetPresetGuid(preset);
    NV_ENC_TUNING_INFO tuning = NV_ENC_TUNING_INFO_HIGH_QUALITY;

    NV_ENC_PRESET_CONFIG presetCfg{};
    presetCfg.version = NV_ENC_PRESET_CONFIG_VER;
    presetCfg.presetCfg.version = NV_ENC_CONFIG_VER;

    st = m_fn.nvEncGetEncodePresetConfigEx(m_encoder, encodeGuid, presetGuid, tuning, &presetCfg);
    if (st != NV_ENC_SUCCESS) {
        LOG_WARN("Codec %d with preset P%d failed (status %d), falling back to H.264.",
                 (int)codec, preset, (int)st);
        m_codec = VideoCodec::H264;
        encodeGuid = NV_ENC_CODEC_H264_GUID;
        st = m_fn.nvEncGetEncodePresetConfigEx(m_encoder, encodeGuid, presetGuid, tuning, &presetCfg);
        if (st != NV_ENC_SUCCESS) {
            LOG_ERROR("nvEncGetEncodePresetConfigEx failed: %d", (int)st);
            return false;
        }
    }

    NV_ENC_CONFIG encCfg = presetCfg.presetCfg;
    uint32_t gopLen = static_cast<uint32_t>(fps); // 1-second GOP for clean 1s granularity cuts
    encCfg.gopLength = gopLen;
    encCfg.frameIntervalP = 1; // IPPPP (zero B-frame reorder so PTS == DTS and zero lookahead buffer complexity)

    uint64_t avgBits = static_cast<uint64_t>(bitrateKbps) * 1000ULL;
    uint64_t maxBits = (avgBits * 3ULL) / 2ULL;

    encCfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    encCfg.rcParams.averageBitRate = static_cast<uint32_t>(avgBits);
    encCfg.rcParams.maxBitRate = static_cast<uint32_t>(maxBits);
    encCfg.rcParams.vbvBufferSize = static_cast<uint32_t>(avgBits); // 1s VBV buffer
    encCfg.rcParams.enableAQ = 1;
    encCfg.rcParams.aqStrength = 8;
    encCfg.rcParams.enableLookahead = 0;
    encCfg.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;

    if (m_codec == VideoCodec::AV1) {
        auto& av1 = encCfg.encodeCodecConfig.av1Config;
        av1.idrPeriod = gopLen;
        av1.repeatSeqHdr = 1;
        av1.outputAnnexBFormat = 0; // Section 5 low-overhead OBU format (required for ISOBMFF av01)
        av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        av1.colorRange = 0;
        av1.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    } else if (m_codec == VideoCodec::HEVC) {
        auto& hevc = encCfg.encodeCodecConfig.hevcConfig;
        hevc.idrPeriod = gopLen;
        hevc.repeatSPSPPS = 1;
        hevc.hevcVUIParameters.colourDescriptionPresentFlag = 1;
        hevc.hevcVUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        hevc.hevcVUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        hevc.hevcVUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        hevc.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        hevc.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    } else {
        auto& h264 = encCfg.encodeCodecConfig.h264Config;
        h264.idrPeriod = gopLen;
        h264.repeatSPSPPS = 1;
        h264.h264VUIParameters.colourDescriptionPresentFlag = 1;
        h264.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
        h264.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
        h264.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        h264.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        h264.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    }

    NV_ENC_INITIALIZE_PARAMS initParams{};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = encodeGuid;
    initParams.presetGUID = presetGuid;
    initParams.encodeWidth = width;
    initParams.encodeHeight = height;
    initParams.darWidth = width;
    initParams.darHeight = height;
    initParams.frameRateNum = fps;
    initParams.frameRateDen = 1;
    initParams.enableEncodeAsync = 0; // Synchronous mode on our dedicated low-priority thread
    initParams.enablePTD = 1;
    initParams.tuningInfo = tuning;
    initParams.encodeConfig = &encCfg;
    initParams.maxEncodeWidth = width;
    initParams.maxEncodeHeight = height;

    st = m_fn.nvEncInitializeEncoder(m_encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("nvEncInitializeEncoder failed: %d (%s)",
                  (int)st, m_fn.nvEncGetLastErrorString ? m_fn.nvEncGetLastErrorString(m_encoder) : "");
        return false;
    }

    // Retrieve Sequence Parameters (SPS/PPS/VPS or AV1 sequence header OBU)
    uint8_t seqBuf[1024]{};
    uint32_t seqOutSize = 0;
    NV_ENC_SEQUENCE_PARAM_PAYLOAD seqPayload{};
    seqPayload.version = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    seqPayload.inBufferSize = sizeof(seqBuf);
    seqPayload.spsppsBuffer = seqBuf;
    seqPayload.outSPSPPSPayloadSize = &seqOutSize;
    if (m_fn.nvEncGetSequenceParams(m_encoder, &seqPayload) == NV_ENC_SUCCESS && seqOutSize > 0) {
        m_seqParams.assign(seqBuf, seqBuf + seqOutSize);
    }

    // Register the shared D3D11 BGRA texture once
    NV_ENC_REGISTER_RESOURCE regRes{};
    regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
    regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regRes.resourceToRegister = sharedInputTex;
    regRes.width = width;
    regRes.height = height;
    regRes.pitch = 0;
    regRes.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB; // DXGI_FORMAT_B8G8R8A8_UNORM maps to NV_ENC_BUFFER_FORMAT_ARGB
    regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

    st = m_fn.nvEncRegisterResource(m_encoder, &regRes);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("nvEncRegisterResource failed: %d", (int)st);
        return false;
    }
    m_registeredRes = regRes.registeredResource;

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER createBs{};
    createBs.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    st = m_fn.nvEncCreateBitstreamBuffer(m_encoder, &createBs);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("nvEncCreateBitstreamBuffer failed: %d", (int)st);
        return false;
    }
    m_bitstreamBuf = createBs.bitstreamBuffer;

    const char* cName = (m_codec == VideoCodec::AV1) ? "AV1" : (m_codec == VideoCodec::HEVC ? "HEVC" : "H.264");
    LOG_INFO("NVENC initialized: %s %dx%d @ %d fps, %d kbps, preset P%d",
             cName, width, height, fps, bitrateKbps, preset);
    return true;
}

bool NvencEncoder::EncodeFrame(uint64_t timestampUs, uint64_t durationUs, bool forceIdr, EncodedVideoPacket& outPkt) {
    if (!m_encoder || !m_registeredRes || !m_bitstreamBuf) return false;

    NV_ENC_MAP_INPUT_RESOURCE mapRes{};
    mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapRes.registeredResource = m_registeredRes;
    NVENCSTATUS st = m_fn.nvEncMapInputResource(m_encoder, &mapRes);
    if (st != NV_ENC_SUCCESS) {
        LOG_ERROR("nvEncMapInputResource failed: %d", (int)st);
        return false;
    }

    NV_ENC_PIC_PARAMS picParams{};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputWidth = m_width;
    picParams.inputHeight = m_height;
    picParams.inputBuffer = mapRes.mappedResource;
    picParams.outputBitstream = m_bitstreamBuf;
    picParams.bufferFmt = mapRes.mappedBufferFmt;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.inputTimeStamp = timestampUs;
    picParams.inputDuration = durationUs;
    if (forceIdr) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    }

    st = m_fn.nvEncEncodePicture(m_encoder, &picParams);
    if (st != NV_ENC_SUCCESS) {
        m_fn.nvEncUnmapInputResource(m_encoder, mapRes.mappedResource);
        if (st == NV_ENC_ERR_NEED_MORE_INPUT) return false;
        LOG_ERROR("nvEncEncodePicture failed: %d", (int)st);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lockBs{};
    lockBs.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockBs.outputBitstream = m_bitstreamBuf;
    lockBs.doNotWait = 0;

    st = m_fn.nvEncLockBitstream(m_encoder, &lockBs);
    if (st == NV_ENC_SUCCESS) {
        const uint8_t* ptr = static_cast<const uint8_t*>(lockBs.bitstreamBufferPtr);
        outPkt.data.assign(ptr, ptr + lockBs.bitstreamSizeInBytes);
        outPkt.timestamp_us = timestampUs;
        outPkt.duration_us = durationUs;
        outPkt.is_keyframe = (lockBs.pictureType == NV_ENC_PIC_TYPE_IDR || lockBs.pictureType == NV_ENC_PIC_TYPE_I);
        m_fn.nvEncUnlockBitstream(m_encoder, m_bitstreamBuf);
    }

    m_fn.nvEncUnmapInputResource(m_encoder, mapRes.mappedResource);
    return (st == NV_ENC_SUCCESS && !outPkt.data.empty());
}
