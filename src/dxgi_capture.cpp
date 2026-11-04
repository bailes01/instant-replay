#include "dxgi_capture.h"
#include "logger.h"

DxgiCapture::~DxgiCapture() {
    Shutdown();
}

void DxgiCapture::Shutdown() {
    m_dupl.Reset();
    m_output1.Reset();
    m_frameTex.Reset();
    m_context.Reset();
    m_device.Reset();
    m_adapter.Reset();
    m_hasFrame = false;
}

bool DxgiCapture::Init(int monitorIndex) {
    Shutdown();
    m_monitorIndex = monitorIndex;
    m_monitors.clear();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("CreateDXGIFactory1 failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // Find NVIDIA adapter (VendorId 0x10DE) or first adapter with outputs
    ComPtr<IDXGIAdapter1> chosenAdapter;
    ComPtr<IDXGIOutput> chosenOutput;
    int globalOutIdx = 0;

    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 adesc{};
        adapter->GetDesc1(&adesc);

        for (UINT o = 0; ; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC odesc{};
            output->GetDesc(&odesc);

            int w = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
            int h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;

            char nameBuf[128]{};
            WideCharToMultiByte(CP_UTF8, 0, odesc.DeviceName, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);

            char gpuBuf[128]{};
            WideCharToMultiByte(CP_UTF8, 0, adesc.Description, -1, gpuBuf, sizeof(gpuBuf), nullptr, nullptr);

            MonitorInfo info;
            info.index = globalOutIdx;
            info.name = std::string(nameBuf) + " (" + std::to_string(w) + "x" + std::to_string(h) + " on " + gpuBuf + ")";
            info.width = w;
            info.height = h;
            info.left = odesc.DesktopCoordinates.left;
            info.top = odesc.DesktopCoordinates.top;
            m_monitors.push_back(info);

            if (globalOutIdx == monitorIndex || !chosenOutput) {
                chosenAdapter = adapter;
                chosenOutput = output;
                m_width = w;
                m_height = h;
                if (globalOutIdx == monitorIndex) {
                    m_monitorIndex = globalOutIdx;
                }
            }
            globalOutIdx++;
        }
    }

    if (!chosenAdapter || !chosenOutput) {
        LOG_ERROR("No display outputs found for Desktop Duplication.");
        return false;
    }

    m_adapter = chosenAdapter;
    hr = chosenOutput.As(&m_output1);
    if (FAILED(hr)) {
        LOG_ERROR("QueryInterface IDXGIOutput1 failed: 0x%08X", (unsigned)hr);
        return false;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL achievedLevel = D3D_FEATURE_LEVEL_11_0;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    hr = D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        createFlags,
        featureLevels,
        2,
        D3D11_SDK_VERSION,
        m_device.GetAddressOf(),
        &achievedLevel,
        m_context.GetAddressOf()
    );
    if (FAILED(hr)) {
        // Fallback without VIDEO_SUPPORT flag if driver dislikes it
        createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(
            m_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createFlags,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            m_device.GetAddressOf(),
            &achievedLevel,
            m_context.GetAddressOf()
        );
        if (FAILED(hr)) {
            LOG_ERROR("D3D11CreateDevice failed: 0x%08X", (unsigned)hr);
            return false;
        }
    }

    // Enable multithread protection on the D3D11 device
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    if (!InitDuplication()) {
        return false;
    }

    // Create the persistent GPU texture that NVENC will register directly
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, m_frameTex.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("CreateTexture2D for shared frame failed: 0x%08X", (unsigned)hr);
        return false;
    }

    // Grab an initial frame (wait up to 250ms, or keep cleared black texture so recording starts immediately even on a static desktop)
    m_hasFrame = true;
    bool dummyReinit = false;
    AcquireFrame(250, dummyReinit);

    LOG_INFO("DXGI Desktop Duplication initialized on monitor %d (%dx%d)", m_monitorIndex, m_width, m_height);
    return true;
}

bool DxgiCapture::InitDuplication() {
    m_dupl.Reset();
    HRESULT hr = m_output1->DuplicateOutput(m_device.Get(), m_dupl.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ERROR("DuplicateOutput failed: 0x%08X (fullscreen transition or UAC prompt?)", (unsigned)hr);
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc{};
    m_dupl->GetDesc(&duplDesc);
    m_width = duplDesc.ModeDesc.Width;
    m_height = duplDesc.ModeDesc.Height;
    return true;
}

bool DxgiCapture::AcquireFrame(int timeoutMs, bool& needReinit) {
    needReinit = false;
    if (!m_dupl) {
        needReinit = true;
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo{};
    ComPtr<IDXGIResource> desktopRes;
    HRESULT hr = m_dupl->AcquireNextFrame(timeoutMs, &frameInfo, desktopRes.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // Desktop did not update during this interval; reuse previous frame texture
        return m_hasFrame;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_DEVICE_REMOVED) {
        LOG_WARN("Desktop duplication access lost (0x%08X); will reinitialize.", (unsigned)hr);
        needReinit = true;
        return false;
    }

    if (FAILED(hr)) {
        return m_hasFrame;
    }

    // Even if only pointer moved (LastPresentTime == 0), Desktop Duplication only gives
    // a non-null desktopRes when desktop image pixels updated.
    ComPtr<ID3D11Texture2D> srcTex;
    if (desktopRes) {
        hr = desktopRes.As(&srcTex);
        if (SUCCEEDED(hr) && srcTex) {
            D3D11_TEXTURE2D_DESC srcDesc{};
            srcTex->GetDesc(&srcDesc);
            if ((int)srcDesc.Width == m_width && (int)srcDesc.Height == m_height &&
                srcDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                m_context->CopyResource(m_frameTex.Get(), srcTex.Get());
                m_hasFrame = true;
            } else if ((int)srcDesc.Width != m_width || (int)srcDesc.Height != m_height) {
                m_dupl->ReleaseFrame();
                needReinit = true;
                return false;
            }
        }
    }

    m_dupl->ReleaseFrame();
    return m_hasFrame;
}
