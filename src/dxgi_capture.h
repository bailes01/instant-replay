#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>

using Microsoft::WRL::ComPtr;

struct MonitorInfo {
    int index;
    std::string name;
    int width;
    int height;
    int left;
    int top;
};

class DxgiCapture {
public:
    DxgiCapture() = default;
    ~DxgiCapture();

    bool Init(int monitorIndex);
    void Shutdown();

    // Captures the next desktop frame or reuses the previous texture if the screen hasn't changed.
    // Returns true if a valid frame is in GetSharedTexture().
    // Returns false and sets needReinit=true if Desktop Duplication lost access (e.g. mode switch).
    bool AcquireFrame(int timeoutMs, bool& needReinit);

    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    ID3D11Texture2D* GetFrameTexture() const { return m_frameTex.Get(); }

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }
    int GetActiveMonitorIndex() const { return m_monitorIndex; }
    const std::vector<MonitorInfo>& GetMonitors() const { return m_monitors; }

private:
    bool InitDuplication();

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<IDXGIOutput1> m_output1;
    ComPtr<IDXGIOutputDuplication> m_dupl;
    ComPtr<ID3D11Texture2D> m_frameTex;

    std::vector<MonitorInfo> m_monitors;
    int m_monitorIndex = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_hasFrame = false;
};
