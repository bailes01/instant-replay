#include "audio_capture.h"
#include "logger.h"
#include <initguid.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <ks.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// Definitions for Windows 10 2004+ Process Loopback API if absent from MinGW headers
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"

enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
};

struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};

enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
};

struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
};
#endif

// Agile completion handler for ActivateAudioInterfaceAsync
class AudioActivationHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    AudioActivationHandler() : m_refCount(1) {
        m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    virtual ~AudioActivationHandler() {
        if (m_event) CloseHandle(m_event);
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = InterlockedDecrement(&m_refCount);
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT hrActivate = E_FAIL;
        ComPtr<IUnknown> punkAudioInterface;
        HRESULT hr = operation->GetActivateResult(&hrActivate, punkAudioInterface.GetAddressOf());
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && punkAudioInterface) {
            punkAudioInterface.As(&m_client);
            m_hr = S_OK;
        } else {
            m_hr = FAILED(hrActivate) ? hrActivate : hr;
        }
        SetEvent(m_event);
        return S_OK;
    }

    bool Wait(DWORD timeoutMs, ComPtr<IAudioClient>& outClient) {
        if (WaitForSingleObject(m_event, timeoutMs) != WAIT_OBJECT_0) return false;
        if (FAILED(m_hr) || !m_client) return false;
        outClient = m_client;
        return true;
    }

private:
    LONG m_refCount;
    HANDLE m_event = nullptr;
    HRESULT m_hr = E_FAIL;
    ComPtr<IAudioClient> m_client;
};

static uint64_t GetQpcMicros() {
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>((now.QuadPart * 1000000ULL) / freq.QuadPart);
}

static DWORD GetActiveWindowProcessId() {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return 0; // Ignore our own tray popup
    return pid;
}

AudioCapture::~AudioCapture() {
    Stop();
}

bool AudioCapture::Start(AudioMode mode, int bitrateKbps, PacketCallback onPacket) {
    Stop();
    if (mode == AudioMode::Off) return true;

    m_mode = mode;
    m_bitrateKbps = bitrateKbps;
    m_onPacket = std::move(onPacket);
    m_running = true;
    m_thread = std::thread(&AudioCapture::ThreadMain, this);
    return true;
}

void AudioCapture::Stop() {
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool AudioCapture::InitAacEncoder() {
    m_aacEncoder.Reset();
    HRESULT hr = CoCreateInstance(CLSID_AACMFTEncoder, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(m_aacEncoder.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("Failed to create Media Foundation AAC Encoder: 0x%08X", (unsigned)hr);
        return false;
    }

    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(inType.GetAddressOf());
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, m_channels * 2);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, m_sampleRate * m_channels * 2);

    hr = m_aacEncoder->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("AAC encoder SetInputType failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // Choose nearest supported AAC bitrate (MF AAC supports 12000, 16000, 20000, 24000 bytes/sec)
    UINT32 bytesPerSec = 20000; // 160 kbps default
    if (m_bitrateKbps >= 180) bytesPerSec = 24000;      // 192 kbps
    else if (m_bitrateKbps >= 140) bytesPerSec = 20000; // 160 kbps
    else if (m_bitrateKbps >= 110) bytesPerSec = 16000; // 128 kbps
    else bytesPerSec = 12000;                           // 96 kbps

    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(outType.GetAddressOf());
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSec);
    outType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // Raw AAC packets (no ADTS)

    hr = m_aacEncoder->SetOutputType(0, outType.Get(), 0);
    if (FAILED(hr)) {
        LOG_ERROR("AAC encoder SetOutputType failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // Extract AudioSpecificConfig (last 2+ bytes of MF_MT_USER_DATA after 12-byte HEAACWAVEINFO)
    ComPtr<IMFMediaType> curOut;
    if (SUCCEEDED(m_aacEncoder->GetOutputCurrentType(0, curOut.GetAddressOf()))) {
        UINT32 blobSize = 0;
        if (SUCCEEDED(curOut->GetBlobSize(MF_MT_USER_DATA, &blobSize)) && blobSize >= 14) {
            std::vector<uint8_t> blob(blobSize);
            if (SUCCEEDED(curOut->GetBlob(MF_MT_USER_DATA, blob.data(), blobSize, &blobSize))) {
                m_asc.assign(blob.begin() + 12, blob.end());
            }
        }
    }

    m_aacEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_aacEncoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AudioCapture::InitProcessLoopback(DWORD targetPid, ComPtr<IAudioClient>& outClient, WAVEFORMATEX** outWfx) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetPid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto* handler = new AudioActivationHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
    HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        handler,
        asyncOp.GetAddressOf()
    );

    if (FAILED(hr) || !handler->Wait(1500, outClient)) {
        handler->Release();
        return false;
    }
    handler->Release();

    // Process loopback does not support GetMixFormat; specify IEEE float 48kHz stereo directly
    WAVEFORMATEX* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    wfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx->nChannels = 2;
    wfx->nSamplesPerSec = 48000;
    wfx->wBitsPerSample = 32;
    wfx->nBlockAlign = (wfx->nChannels * wfx->wBitsPerSample) / 8;
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize = 0;

    hr = outClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        200000, // 20 ms buffer
        0,
        wfx,
        nullptr
    );
    if (FAILED(hr)) {
        CoTaskMemFree(wfx);
        return false;
    }

    *outWfx = wfx;
    return true;
}

bool AudioCapture::InitDesktopLoopback(ComPtr<IAudioClient>& outClient, WAVEFORMATEX** outWfx) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(outClient.GetAddressOf()));
    if (FAILED(hr)) return false;

    WAVEFORMATEX* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    wfx->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx->nChannels = 2;
    wfx->nSamplesPerSec = 48000;
    wfx->wBitsPerSample = 32;
    wfx->nBlockAlign = 8;
    wfx->nAvgBytesPerSec = 48000 * 8;
    wfx->cbSize = 0;

    hr = outClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        200000,
        0,
        wfx,
        nullptr
    );
    if (FAILED(hr)) {
        CoTaskMemFree(wfx);
        return false;
    }

    *outWfx = wfx;
    return true;
}

void AudioCapture::FeedPcmToAac(const int16_t* samples, size_t frameCount, uint64_t baseTimestampUs) {
    if (!m_aacEncoder) return;

    if (m_pcmBuffer.empty() || std::llabs((int64_t)baseTimestampUs - (int64_t)m_nextAacPtsUs) > 100000) {
        m_nextAacPtsUs = baseTimestampUs;
    }

    m_pcmBuffer.insert(m_pcmBuffer.end(), samples, samples + frameCount * m_channels);

    const size_t kFrameSamples = 1024;
    const size_t kSamplesPerBlock = kFrameSamples * m_channels;

    while (m_pcmBuffer.size() >= kSamplesPerBlock) {
        DWORD cbBytes = static_cast<DWORD>(kSamplesPerBlock * sizeof(int16_t));
        ComPtr<IMFMediaBuffer> inBuf;
        if (SUCCEEDED(MFCreateMemoryBuffer(cbBytes, inBuf.GetAddressOf()))) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(inBuf->Lock(&dst, nullptr, nullptr))) {
                memcpy(dst, m_pcmBuffer.data(), cbBytes);
                inBuf->Unlock();
                inBuf->SetCurrentLength(cbBytes);
            }
            ComPtr<IMFSample> inSample;
            if (SUCCEEDED(MFCreateSample(inSample.GetAddressOf()))) {
                inSample->AddBuffer(inBuf.Get());
                inSample->SetSampleTime(static_cast<LONGLONG>(m_nextAacPtsUs * 10));
                uint64_t durUs = (kFrameSamples * 1000000ULL) / m_sampleRate;
                inSample->SetSampleDuration(static_cast<LONGLONG>(durUs * 10));
                m_aacEncoder->ProcessInput(0, inSample.Get(), 0);
            }
        }

        m_pcmBuffer.erase(m_pcmBuffer.begin(), m_pcmBuffer.begin() + kSamplesPerBlock);
        uint64_t durUs = (kFrameSamples * 1000000ULL) / m_sampleRate;
        uint64_t pktPtsUs = m_nextAacPtsUs;
        m_nextAacPtsUs += durUs;

        // Pull all available AAC output frames
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER outData{};
            ComPtr<IMFSample> outSample;
            MFCreateSample(outSample.GetAddressOf());
            ComPtr<IMFMediaBuffer> outBuf;
            MFCreateMemoryBuffer(4096, outBuf.GetAddressOf());
            outSample->AddBuffer(outBuf.Get());
            outData.pSample = outSample.Get();

            DWORD status = 0;
            HRESULT hr = m_aacEncoder->ProcessOutput(0, 1, &outData, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(hr)) break;

            ComPtr<IMFMediaBuffer> contig;
            if (SUCCEEDED(outSample->ConvertToContiguousBuffer(contig.GetAddressOf()))) {
                BYTE* ptr = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contig->Lock(&ptr, nullptr, &len)) && len > 0) {
                    EncodedAudioPacket pkt;
                    pkt.data.assign(ptr, ptr + len);
                    LONGLONG sampleTime100ns = 0;
                    if (SUCCEEDED(outSample->GetSampleTime(&sampleTime100ns)) && sampleTime100ns > 0) {
                        pkt.timestamp_us = static_cast<uint64_t>(sampleTime100ns / 10);
                    } else {
                        pkt.timestamp_us = pktPtsUs;
                    }
                    pkt.duration_us = durUs;
                    contig->Unlock();
                    if (m_onPacket) m_onPacket(pkt);
                }
            }
        }
    }
}

void AudioCapture::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    if (!InitAacEncoder()) {
        MFShutdown();
        CoUninitialize();
        return;
    }

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* wfx = nullptr;
    DWORD currentPid = 0;
    uint64_t lastPidCheckUs = 0;

    auto releaseClient = [&]() {
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
        if (wfx) {
            CoTaskMemFree(wfx);
            wfx = nullptr;
        }
    };

    if (m_mode == AudioMode::Desktop) {
        if (InitDesktopLoopback(client, &wfx)) {
            client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.GetAddressOf()));
            client->Start();
            LOG_INFO("WASAPI Desktop Audio Loopback started (48kHz stereo)");
        }
    }

    std::vector<int16_t> convBuf;
    std::vector<int16_t> silenceBuf(480 * 2, 0); // 10 ms stereo silence

    while (m_running) {
        uint64_t nowUs = GetQpcMicros();

        if (m_mode == AudioMode::ActiveWindow && (nowUs - lastPidCheckUs >= 250000 || !client)) {
            lastPidCheckUs = nowUs;
            DWORD fgPid = GetActiveWindowProcessId();
            if (fgPid != 0 && fgPid != currentPid) {
                releaseClient();
                if (InitProcessLoopback(fgPid, client, &wfx)) {
                    client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(capture.GetAddressOf()));
                    client->Start();
                    currentPid = fgPid;
                    LOG_INFO("Audio capture attached to active window process PID %lu", (unsigned long)currentPid);
                } else {
                    currentPid = 0;
                }
            }
        }

        bool gotFrames = false;
        if (capture) {
            UINT32 packetLength = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packetLength)) && packetLength > 0) {
                BYTE* data = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                if (SUCCEEDED(capture->GetBuffer(&data, &numFrames, &flags, nullptr, nullptr))) {
                    convBuf.resize(numFrames * m_channels);
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                        std::fill(convBuf.begin(), convBuf.end(), 0);
                    } else {
                        const float* src = reinterpret_cast<const float*>(data);
                        for (size_t i = 0; i < numFrames * m_channels; ++i) {
                            float s = std::clamp(src[i], -1.0f, 1.0f);
                            convBuf[i] = static_cast<int16_t>(s * 32767.0f);
                        }
                    }
                    capture->ReleaseBuffer(numFrames);
                    FeedPcmToAac(convBuf.data(), numFrames, nowUs);
                    gotFrames = true;
                } else {
                    break;
                }
            }
        }

        if (!gotFrames) {
            // WASAPI loopback stops emitting packets when the target process is silent;
            // feed continuous silence after 20ms so AAC timestamps stay continuous.
            Sleep(10);
            uint64_t afterSleepUs = GetQpcMicros();
            if (!capture) {
                FeedPcmToAac(silenceBuf.data(), 480, afterSleepUs);
            } else {
                UINT32 pktLen = 0;
                if (SUCCEEDED(capture->GetNextPacketSize(&pktLen)) && pktLen == 0) {
                    FeedPcmToAac(silenceBuf.data(), 480, afterSleepUs);
                }
            }
        } else {
            Sleep(5);
        }
    }

    releaseClient();
    m_aacEncoder.Reset();
    MFShutdown();
    CoUninitialize();
}
