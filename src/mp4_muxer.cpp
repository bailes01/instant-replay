#include "mp4_muxer.h"
#include "logger.h"
#include <fstream>
#include <cstring>
#include <algorithm>

namespace {

struct ByteWriter {
    std::vector<uint8_t> buf;

    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) {
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void u24(uint32_t v) {
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void u32(uint32_t v) {
        buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void u64(uint64_t v) {
        u32(static_cast<uint32_t>(v >> 32));
        u32(static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    }
    void fourcc(const char (&cc)[5]) {
        buf.insert(buf.end(), cc, cc + 4);
    }
    void bytes(const uint8_t* p, size_t n) {
        buf.insert(buf.end(), p, p + n);
    }
    void bytes(const std::vector<uint8_t>& v) {
        buf.insert(buf.end(), v.begin(), v.end());
    }
    void zeros(size_t n) {
        buf.insert(buf.end(), n, 0);
    }
    size_t beginBox(const char (&cc)[5]) {
        size_t pos = buf.size();
        u32(0);
        fourcc(cc);
        return pos;
    }
    size_t beginFullBox(const char (&cc)[5], uint8_t version, uint32_t flags) {
        size_t pos = beginBox(cc);
        u8(version);
        u24(flags);
        return pos;
    }
    void endBox(size_t pos) {
        uint32_t sz = static_cast<uint32_t>(buf.size() - pos);
        buf[pos + 0] = static_cast<uint8_t>((sz >> 24) & 0xFF);
        buf[pos + 1] = static_cast<uint8_t>((sz >> 16) & 0xFF);
        buf[pos + 2] = static_cast<uint8_t>((sz >> 8) & 0xFF);
        buf[pos + 3] = static_cast<uint8_t>(sz & 0xFF);
    }
};

struct NalUnit {
    const uint8_t* data;
    size_t size;
};

static std::vector<NalUnit> SplitAnnexBNals(const uint8_t* data, size_t len) {
    std::vector<NalUnit> nals;
    size_t i = 0;
    while (i + 3 < len) {
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) scLen = 3;
        else if (i + 4 <= len && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) scLen = 4;
        if (scLen == 0) {
            i++;
            continue;
        }
        size_t nalStart = i + scLen;
        size_t j = nalStart;
        while (j + 3 <= len) {
            if (data[j] == 0 && data[j + 1] == 0 &&
                (data[j + 2] == 1 || (j + 3 < len && data[j + 2] == 0 && data[j + 3] == 1))) {
                break;
            }
            j++;
        }
        if (j + 3 > len) j = len;
        if (j > nalStart) {
            nals.push_back({ data + nalStart, j - nalStart });
        }
        i = j;
    }
    return nals;
}

static std::vector<uint8_t> AnnexBToLengthPrefixed(const std::vector<uint8_t>& annexB) {
    auto nals = SplitAnnexBNals(annexB.data(), annexB.size());
    std::vector<uint8_t> out;
    out.reserve(annexB.size() + 16);
    for (const auto& n : nals) {
        uint32_t sz = static_cast<uint32_t>(n.size);
        out.push_back(static_cast<uint8_t>((sz >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(sz & 0xFF));
        out.insert(out.end(), n.data, n.data + n.size);
    }
    return out;
}

// Extract AV1 Sequence Header OBU (type == 1) from OBU stream
static std::vector<uint8_t> ExtractAv1SeqHdrObu(const std::vector<uint8_t>& obuStream) {
    size_t pos = 0;
    while (pos < obuStream.size()) {
        size_t obuStart = pos;
        uint8_t hdr = obuStream[pos++];
        uint8_t obuType = (hdr >> 3) & 0x0F;
        bool hasExt = (hdr & 0x04) != 0;
        bool hasSize = (hdr & 0x02) != 0;
        if (hasExt) {
            if (pos >= obuStream.size()) break;
            pos++;
        }
        if (!hasSize) break;
        uint64_t payloadLen = 0;
        for (int b = 0; b < 8 && pos < obuStream.size(); ++b) {
            uint8_t byte = obuStream[pos++];
            payloadLen |= static_cast<uint64_t>(byte & 0x7F) << (b * 7);
            if ((byte & 0x80) == 0) break;
        }
        if (pos + payloadLen > obuStream.size()) break;
        size_t obuEnd = pos + static_cast<size_t>(payloadLen);
        if (obuType == 1) { // OBU_SEQUENCE_HEADER
            return std::vector<uint8_t>(obuStream.begin() + obuStart, obuStream.begin() + obuEnd);
        }
        pos = obuEnd;
    }
    return {};
}

static void WriteAv1CBox(ByteWriter& w, const std::vector<uint8_t>& seqHdrObu) {
    size_t p = w.beginBox("av1C");
    w.u8(0x81); // marker = 1, version = 1
    w.u8(0x08); // seq_profile = 0 (Main), seq_level_idx_0 = 8 (Level 4.0)
    w.u8(0x0C); // seq_tier_0=0, high_bitdepth=0, twelve_bit=0, monochrome=0, chroma_subsampling_x=1, y=1, pos=0
    w.u8(0x00); // initial_presentation_delay_present = 0
    if (!seqHdrObu.empty()) {
        w.bytes(seqHdrObu);
    }
    w.endBox(p);
}

static void WriteAvcCBox(ByteWriter& w, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    size_t p = w.beginBox("avcC");
    w.u8(1); // configurationVersion = 1
    w.u8(sps.size() >= 4 ? sps[1] : 0x64); // AVCProfileIndication (High)
    w.u8(sps.size() >= 4 ? sps[2] : 0x00); // profile_compatibility
    w.u8(sps.size() >= 4 ? sps[3] : 0x2A); // AVCLevelIndication (4.2)
    w.u8(0xFF); // 4-byte NAL unit lengths (lengthSizeMinusOne = 3)
    if (!sps.empty()) {
        w.u8(0xE1); // numOfSequenceParameterSets = 1
        w.u16(static_cast<uint16_t>(sps.size()));
        w.bytes(sps);
    } else {
        w.u8(0xE0);
    }
    if (!pps.empty()) {
        w.u8(1); // numOfPictureParameterSets = 1
        w.u16(static_cast<uint16_t>(pps.size()));
        w.bytes(pps);
    } else {
        w.u8(0);
    }
    w.endBox(p);
}

static void WriteHvcCBox(ByteWriter& w, const std::vector<uint8_t>& vps,
                         const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    size_t p = w.beginBox("hvcC");
    w.u8(1);          // configurationVersion = 1
    w.u8(0x01);       // general_profile_space=0, tier=0, profile_idc=1 (Main)
    w.u32(0x60000000);// general_profile_compatibility_flags
    w.u32(0xB0000000);// general_constraint_indicator_flags (top 32)
    w.u16(0x0000);    // general_constraint_indicator_flags (low 16)
    w.u8(123);        // general_level_idc (Level 4.1 = 123)
    w.u16(0xF000);    // min_spatial_segmentation_idc = 0
    w.u8(0xFC);       // parallelismType = 0
    w.u8(0xFD);       // chromaFormat = 1 (4:2:0)
    w.u8(0xF8);       // bitDepthLumaMinus8 = 0
    w.u8(0xF8);       // bitDepthChromaMinus8 = 0
    w.u16(0);         // avgFrameRate = 0
    w.u8(0x0F);       // constantFrameRate=0, numTemporalLayers=1, temporalIdNested=1, lengthSizeMinusOne=3

    uint8_t numArrays = (!vps.empty() ? 1 : 0) + (!sps.empty() ? 1 : 0) + (!pps.empty() ? 1 : 0);
    w.u8(numArrays);

    auto writeArray = [&](uint8_t nalType, const std::vector<uint8_t>& nal) {
        if (nal.empty()) return;
        w.u8(0x80 | (nalType & 0x3F));
        w.u16(1);
        w.u16(static_cast<uint16_t>(nal.size()));
        w.bytes(nal);
    };
    writeArray(32, vps);
    writeArray(33, sps);
    writeArray(34, pps);
    w.endBox(p);
}

static void WriteEsdsBox(ByteWriter& w, const std::vector<uint8_t>& asc) {
    size_t p = w.beginFullBox("esds", 0, 0);
    const std::vector<uint8_t> cfg = asc.empty() ? std::vector<uint8_t>{ 0x11, 0x90 } : asc;

    uint8_t decSpecLen = static_cast<uint8_t>(cfg.size());
    uint8_t decCfgLen = static_cast<uint8_t>(13 + 2 + decSpecLen);
    uint8_t esDescLen = static_cast<uint8_t>(3 + 2 + decCfgLen + 3);

    // ES_Descriptor (tag 0x03)
    w.u8(0x03);
    w.u8(esDescLen);
    w.u16(0x0002); // ES_ID = 2
    w.u8(0x00);    // flags = 0

    // DecoderConfigDescriptor (tag 0x04)
    w.u8(0x04);
    w.u8(decCfgLen);
    w.u8(0x40);    // objectTypeIndication = 0x40 (Audio ISO/IEC 14496-3)
    w.u8(0x15);    // streamType = 0x05 (AudioStream) << 2 | 1
    w.u24(0);      // bufferSizeDB
    w.u32(192000); // maxBitrate
    w.u32(160000); // avgBitrate

    // DecoderSpecificInfo (tag 0x05)
    w.u8(0x05);
    w.u8(decSpecLen);
    w.bytes(cfg);

    // SLConfigDescriptor (tag 0x06)
    w.u8(0x06);
    w.u8(0x01);
    w.u8(0x02);
    w.endBox(p);
}

} // namespace

bool Mp4Muxer::WriteClip(
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
) {
    if (snapshot.video_packets.empty()) {
        LOG_WARN("Mp4Muxer::WriteClip called with empty video snapshot.");
        return false;
    }

    const uint32_t kVideoTimescale = 90000;
    const uint32_t kAudioTimescale = static_cast<uint32_t>(audioSampleRate > 0 ? audioSampleRate : 48000);

    // Prepare video payloads and codec configuration data
    std::vector<std::vector<uint8_t>> videoPayloads;
    videoPayloads.reserve(snapshot.video_packets.size());
    std::vector<uint32_t> videoSizes;
    std::vector<uint32_t> videoDeltas;
    std::vector<uint32_t> syncSamples;

    std::vector<uint8_t> sps, pps, vps, av1SeqObu;

    auto scanAnnexBForParams = [&](const std::vector<uint8_t>& buf) {
        for (const auto& n : SplitAnnexBNals(buf.data(), buf.size())) {
            if (n.size == 0) continue;
            if (codec == VideoCodec::H264) {
                uint8_t nalType = n.data[0] & 0x1F;
                if (nalType == 7 && sps.empty()) sps.assign(n.data, n.data + n.size);
                else if (nalType == 8 && pps.empty()) pps.assign(n.data, n.data + n.size);
            } else if (codec == VideoCodec::HEVC) {
                uint8_t nalType = (n.data[0] >> 1) & 0x3F;
                if (nalType == 32 && vps.empty()) vps.assign(n.data, n.data + n.size);
                else if (nalType == 33 && sps.empty()) sps.assign(n.data, n.data + n.size);
                else if (nalType == 34 && pps.empty()) pps.assign(n.data, n.data + n.size);
            }
        }
    };

    if (!videoSeqParams.empty()) {
        if (codec == VideoCodec::AV1) {
            av1SeqObu = ExtractAv1SeqHdrObu(videoSeqParams);
        } else {
            scanAnnexBForParams(videoSeqParams);
        }
    }

    uint64_t totalVideoTicks = 0;
    for (size_t i = 0; i < snapshot.video_packets.size(); ++i) {
        const auto& pkt = snapshot.video_packets[i];
        if (codec == VideoCodec::AV1) {
            if (av1SeqObu.empty()) av1SeqObu = ExtractAv1SeqHdrObu(pkt.data);
            videoPayloads.push_back(pkt.data);
        } else {
            if (sps.empty() || pps.empty()) scanAnnexBForParams(pkt.data);
            videoPayloads.push_back(AnnexBToLengthPrefixed(pkt.data));
        }
        videoSizes.push_back(static_cast<uint32_t>(videoPayloads.back().size()));

        uint64_t durUs = (i + 1 < snapshot.video_packets.size())
            ? (snapshot.video_packets[i + 1].timestamp_us - pkt.timestamp_us)
            : pkt.duration_us;
        if (durUs == 0 || durUs > 1000000ULL) durUs = 1000000ULL / (fps > 0 ? fps : 60);
        uint32_t delta = static_cast<uint32_t>((durUs * kVideoTimescale + 500000ULL) / 1000000ULL);
        if (delta == 0) delta = 1;
        videoDeltas.push_back(delta);
        totalVideoTicks += delta;

        if (pkt.is_keyframe) {
            syncSamples.push_back(static_cast<uint32_t>(i + 1)); // 1-based sample index
        }
    }

    bool hasAudio = !snapshot.audio_packets.empty();
    std::vector<uint32_t> audioSizes;
    std::vector<uint32_t> audioDeltas;
    uint64_t totalAudioTicks = 0;
    for (const auto& apkt : snapshot.audio_packets) {
        audioSizes.push_back(static_cast<uint32_t>(apkt.data.size()));
        audioDeltas.push_back(1024); // AAC-LC always 1024 samples per frame
        totalAudioTicks += 1024;
    }

    // Build ftyp box
    ByteWriter ftyp;
    {
        size_t p = ftyp.beginBox("ftyp");
        ftyp.fourcc("isom");
        ftyp.u32(0x200);
        ftyp.fourcc("isom");
        ftyp.fourcc("iso2");
        if (codec == VideoCodec::AV1) ftyp.fourcc("av01");
        else if (codec == VideoCodec::HEVC) ftyp.fourcc("hev1");
        else ftyp.fourcc("avc1");
        ftyp.fourcc("mp41");
        ftyp.endBox(p);
    }

    // Helper that builds the entire moov box given exact file offsets for each video and audio sample
    auto buildMoov = [&](const std::vector<uint32_t>& videoOffsets,
                         const std::vector<uint32_t>& audioOffsets) -> std::vector<uint8_t> {
        ByteWriter moov;
        size_t pMoov = moov.beginBox("moov");

        uint32_t movieTimescale = 1000;
        uint32_t movieDurationMs = static_cast<uint32_t>((totalVideoTicks * 1000ULL) / kVideoTimescale);

        // mvhd
        {
            size_t pMvhd = moov.beginFullBox("mvhd", 0, 0);
            moov.u32(0); // creation_time
            moov.u32(0); // modification_time
            moov.u32(movieTimescale);
            moov.u32(movieDurationMs);
            moov.u32(0x00010000); // rate = 1.0
            moov.u16(0x0100);     // volume = 1.0
            moov.zeros(10);       // reserved
            // Unity matrix
            uint32_t matrix[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
            for (uint32_t m : matrix) moov.u32(m);
            moov.zeros(24);       // pre_defined
            moov.u32(hasAudio ? 3 : 2); // next_track_ID
            moov.endBox(pMvhd);
        }

        // Video trak (track_ID = 1)
        {
            size_t pTrak = moov.beginBox("trak");
            {
                size_t pTkhd = moov.beginFullBox("tkhd", 0, 0x000003); // enabled | in_movie
                moov.u32(0);
                moov.u32(0);
                moov.u32(1); // track_ID = 1
                moov.u32(0);
                moov.u32(movieDurationMs);
                moov.zeros(8);
                moov.u16(0); // layer
                moov.u16(0); // alternate_group
                moov.u16(0); // volume (0 for video)
                moov.u16(0);
                uint32_t matrix[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
                for (uint32_t m : matrix) moov.u32(m);
                moov.u32(static_cast<uint32_t>(width) << 16);
                moov.u32(static_cast<uint32_t>(height) << 16);
                moov.endBox(pTkhd);
            }
            {
                size_t pMdia = moov.beginBox("mdia");
                {
                    size_t pMdhd = moov.beginFullBox("mdhd", 0, 0);
                    moov.u32(0);
                    moov.u32(0);
                    moov.u32(kVideoTimescale);
                    moov.u32(static_cast<uint32_t>(totalVideoTicks));
                    moov.u16(0x55C4); // 'und' language
                    moov.u16(0);
                    moov.endBox(pMdhd);
                }
                {
                    size_t pHdlr = moov.beginFullBox("hdlr", 0, 0);
                    moov.u32(0);
                    moov.fourcc("vide");
                    moov.zeros(12);
                    const char name[] = "InstantReplay Video";
                    moov.bytes(reinterpret_cast<const uint8_t*>(name), sizeof(name));
                    moov.endBox(pHdlr);
                }
                {
                    size_t pMinf = moov.beginBox("minf");
                    {
                        size_t pVmhd = moov.beginFullBox("vmhd", 0, 1);
                        moov.u16(0);
                        moov.zeros(6);
                        moov.endBox(pVmhd);
                    }
                    {
                        size_t pDinf = moov.beginBox("dinf");
                        size_t pDref = moov.beginFullBox("dref", 0, 0);
                        moov.u32(1);
                        size_t pUrl = moov.beginFullBox("url ", 0, 1);
                        moov.endBox(pUrl);
                        moov.endBox(pDref);
                        moov.endBox(pDinf);
                    }
                    {
                        size_t pStbl = moov.beginBox("stbl");
                        // stsd
                        {
                            size_t pStsd = moov.beginFullBox("stsd", 0, 0);
                            moov.u32(1);
                            const char (&sample4cc)[5] = (codec == VideoCodec::AV1) ? "av01" :
                                                         (codec == VideoCodec::HEVC ? "hev1" : "avc1");
                            size_t pEntry = moov.beginBox(sample4cc);
                            moov.zeros(6);
                            moov.u16(1); // data_reference_index = 1
                            moov.zeros(16);
                            moov.u16(static_cast<uint16_t>(width));
                            moov.u16(static_cast<uint16_t>(height));
                            moov.u32(0x00480000); // 72 dpi
                            moov.u32(0x00480000);
                            moov.u32(0);
                            moov.u16(1); // frame_count = 1
                            moov.zeros(32); // compressorname
                            moov.u16(0x0018); // depth = 24
                            moov.u16(0xFFFF); // pre_defined = -1
                            if (codec == VideoCodec::AV1) {
                                WriteAv1CBox(moov, av1SeqObu);
                            } else if (codec == VideoCodec::HEVC) {
                                WriteHvcCBox(moov, vps, sps, pps);
                            } else {
                                WriteAvcCBox(moov, sps, pps);
                            }
                            moov.endBox(pEntry);
                            moov.endBox(pStsd);
                        }
                        // stts
                        {
                            size_t pStts = moov.beginFullBox("stts", 0, 0);
                            moov.u32(static_cast<uint32_t>(videoDeltas.size()));
                            for (uint32_t d : videoDeltas) {
                                moov.u32(1);
                                moov.u32(d);
                            }
                            moov.endBox(pStts);
                        }
                        // stss
                        {
                            size_t pStss = moov.beginFullBox("stss", 0, 0);
                            moov.u32(static_cast<uint32_t>(syncSamples.size()));
                            for (uint32_t s : syncSamples) moov.u32(s);
                            moov.endBox(pStss);
                        }
                        // stsc (1 sample per chunk for chronological A/V interleaving)
                        {
                            size_t pStsc = moov.beginFullBox("stsc", 0, 0);
                            moov.u32(1);
                            moov.u32(1); // first_chunk = 1
                            moov.u32(1); // samples_per_chunk = 1
                            moov.u32(1); // sample_description_index = 1
                            moov.endBox(pStsc);
                        }
                        // stsz
                        {
                            size_t pStsz = moov.beginFullBox("stsz", 0, 0);
                            moov.u32(0);
                            moov.u32(static_cast<uint32_t>(videoSizes.size()));
                            for (uint32_t sz : videoSizes) moov.u32(sz);
                            moov.endBox(pStsz);
                        }
                        // stco
                        {
                            size_t pStco = moov.beginFullBox("stco", 0, 0);
                            moov.u32(static_cast<uint32_t>(videoOffsets.size()));
                            for (uint32_t off : videoOffsets) moov.u32(off);
                            moov.endBox(pStco);
                        }
                        moov.endBox(pStbl);
                    }
                    moov.endBox(pMinf);
                }
                moov.endBox(pMdia);
            }
            moov.endBox(pTrak);
        }

        // Audio trak (track_ID = 2)
        if (hasAudio) {
            uint32_t audioMovieDurMs = static_cast<uint32_t>((totalAudioTicks * 1000ULL) / kAudioTimescale);
            size_t pTrak = moov.beginBox("trak");
            {
                size_t pTkhd = moov.beginFullBox("tkhd", 0, 0x000003);
                moov.u32(0);
                moov.u32(0);
                moov.u32(2); // track_ID = 2
                moov.u32(0);
                moov.u32(audioMovieDurMs);
                moov.zeros(8);
                moov.u16(0);
                moov.u16(0);
                moov.u16(0x0100); // volume = 1.0
                moov.u16(0);
                uint32_t matrix[9] = { 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000 };
                for (uint32_t m : matrix) moov.u32(m);
                moov.u32(0);
                moov.u32(0);
                moov.endBox(pTkhd);
            }
            {
                size_t pMdia = moov.beginBox("mdia");
                {
                    size_t pMdhd = moov.beginFullBox("mdhd", 0, 0);
                    moov.u32(0);
                    moov.u32(0);
                    moov.u32(kAudioTimescale);
                    moov.u32(static_cast<uint32_t>(totalAudioTicks));
                    moov.u16(0x55C4);
                    moov.u16(0);
                    moov.endBox(pMdhd);
                }
                {
                    size_t pHdlr = moov.beginFullBox("hdlr", 0, 0);
                    moov.u32(0);
                    moov.fourcc("soun");
                    moov.zeros(12);
                    const char name[] = "InstantReplay Audio";
                    moov.bytes(reinterpret_cast<const uint8_t*>(name), sizeof(name));
                    moov.endBox(pHdlr);
                }
                {
                    size_t pMinf = moov.beginBox("minf");
                    {
                        size_t pSmhd = moov.beginFullBox("smhd", 0, 0);
                        moov.u16(0);
                        moov.u16(0);
                        moov.endBox(pSmhd);
                    }
                    {
                        size_t pDinf = moov.beginBox("dinf");
                        size_t pDref = moov.beginFullBox("dref", 0, 0);
                        moov.u32(1);
                        size_t pUrl = moov.beginFullBox("url ", 0, 1);
                        moov.endBox(pUrl);
                        moov.endBox(pDref);
                        moov.endBox(pDinf);
                    }
                    {
                        size_t pStbl = moov.beginBox("stbl");
                        // stsd (mp4a + esds)
                        {
                            size_t pStsd = moov.beginFullBox("stsd", 0, 0);
                            moov.u32(1);
                            size_t pMp4a = moov.beginBox("mp4a");
                            moov.zeros(6);
                            moov.u16(1);
                            moov.zeros(8);
                            moov.u16(static_cast<uint16_t>(audioChannels > 0 ? audioChannels : 2));
                            moov.u16(16);
                            moov.u16(0);
                            moov.u16(0);
                            moov.u32(kAudioTimescale << 16);
                            WriteEsdsBox(moov, audioSpecificConfig);
                            moov.endBox(pMp4a);
                            moov.endBox(pStsd);
                        }
                        // stts (all frames 1024 samples)
                        {
                            size_t pStts = moov.beginFullBox("stts", 0, 0);
                            moov.u32(1);
                            moov.u32(static_cast<uint32_t>(audioDeltas.size()));
                            moov.u32(1024);
                            moov.endBox(pStts);
                        }
                        // stsc (1 sample per chunk for chronological A/V interleaving)
                        {
                            size_t pStsc = moov.beginFullBox("stsc", 0, 0);
                            moov.u32(1);
                            moov.u32(1);
                            moov.u32(1);
                            moov.u32(1);
                            moov.endBox(pStsc);
                        }
                        // stsz
                        {
                            size_t pStsz = moov.beginFullBox("stsz", 0, 0);
                            moov.u32(0);
                            moov.u32(static_cast<uint32_t>(audioSizes.size()));
                            for (uint32_t sz : audioSizes) moov.u32(sz);
                            moov.endBox(pStsz);
                        }
                        // stco
                        {
                            size_t pStco = moov.beginFullBox("stco", 0, 0);
                            moov.u32(static_cast<uint32_t>(audioOffsets.size()));
                            for (uint32_t off : audioOffsets) moov.u32(off);
                            moov.endBox(pStco);
                        }
                        moov.endBox(pStbl);
                    }
                    moov.endBox(pMinf);
                }
                moov.endBox(pMdia);
            }
            moov.endBox(pTrak);
        }

        moov.endBox(pMoov);
        return moov.buf;
    };

    // Pass 1: compute exact size of moov
    std::vector<uint32_t> videoOffsets(videoSizes.size(), 0);
    std::vector<uint32_t> audioOffsets(audioSizes.size(), 0);
    auto dummyMoov = buildMoov(videoOffsets, audioOffsets);

    uint32_t totalVideoBytes = 0;
    for (uint32_t sz : videoSizes) totalVideoBytes += sz;
    uint32_t totalAudioBytes = 0;
    for (uint32_t sz : audioSizes) totalAudioBytes += sz;

    uint32_t mdatHeaderOffset = static_cast<uint32_t>(ftyp.buf.size() + dummyMoov.size());
    uint32_t curOffset = mdatHeaderOffset + 8; // after 8-byte mdat header

    // Compute interleaved sample offsets in chronological order by timestamp_us
    {
        size_t vIdx = 0, aIdx = 0;
        while (vIdx < videoSizes.size() || aIdx < audioSizes.size()) {
            bool takeVideo = false;
            if (vIdx < videoSizes.size() && aIdx < audioSizes.size()) {
                takeVideo = (snapshot.video_packets[vIdx].timestamp_us <= snapshot.audio_packets[aIdx].timestamp_us);
            } else {
                takeVideo = (vIdx < videoSizes.size());
            }
            if (takeVideo) {
                videoOffsets[vIdx] = curOffset;
                curOffset += videoSizes[vIdx];
                vIdx++;
            } else {
                audioOffsets[aIdx] = curOffset;
                curOffset += audioSizes[aIdx];
                aIdx++;
            }
        }
    }

    // Pass 2: build final moov with exact interleaved offsets
    auto finalMoov = buildMoov(videoOffsets, audioOffsets);

    std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LOG_ERROR("Failed to open output MP4 '%s' for writing.", filePath.c_str());
        return false;
    }

    out.write(reinterpret_cast<const char*>(ftyp.buf.data()), ftyp.buf.size());
    out.write(reinterpret_cast<const char*>(finalMoov.data()), finalMoov.size());

    // Write mdat header + chronologically interleaved video & audio samples
    uint32_t mdatSize = 8 + totalVideoBytes + totalAudioBytes;
    uint8_t mdatHdr[8] = {
        static_cast<uint8_t>((mdatSize >> 24) & 0xFF),
        static_cast<uint8_t>((mdatSize >> 16) & 0xFF),
        static_cast<uint8_t>((mdatSize >> 8) & 0xFF),
        static_cast<uint8_t>(mdatSize & 0xFF),
        'm', 'd', 'a', 't'
    };
    out.write(reinterpret_cast<const char*>(mdatHdr), 8);

    {
        size_t vIdx = 0, aIdx = 0;
        while (vIdx < videoPayloads.size() || aIdx < snapshot.audio_packets.size()) {
            bool takeVideo = false;
            if (vIdx < videoPayloads.size() && aIdx < snapshot.audio_packets.size()) {
                takeVideo = (snapshot.video_packets[vIdx].timestamp_us <= snapshot.audio_packets[aIdx].timestamp_us);
            } else {
                takeVideo = (vIdx < videoPayloads.size());
            }
            if (takeVideo) {
                out.write(reinterpret_cast<const char*>(videoPayloads[vIdx].data()), videoPayloads[vIdx].size());
                vIdx++;
            } else {
                out.write(reinterpret_cast<const char*>(snapshot.audio_packets[aIdx].data.data()),
                          snapshot.audio_packets[aIdx].data.size());
                aIdx++;
            }
        }
    }

    out.close();
    double durationSec = static_cast<double>(totalVideoTicks) / kVideoTimescale;
    LOG_INFO("Saved MP4 '%s' (%.1fs, %zu video frames, %zu audio frames, %.2f MB)",
             filePath.c_str(), durationSec, videoPayloads.size(), snapshot.audio_packets.size(),
             (ftyp.buf.size() + finalMoov.size() + mdatSize) / (1024.0 * 1024.0));
    return true;
}
