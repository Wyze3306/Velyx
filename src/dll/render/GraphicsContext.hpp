#pragma once

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dwrite_3.h>
#include <dxgi1_4.h>

#include <mutex>
#include <vector>

#include "core/Math.hpp"
#include "dll/render/ComPtr.hpp"

namespace velyx {

class GraphicsContext {
public:
    enum class Backend { None, D3D11, D3D12 };

    static GraphicsContext& get();

    void setCommandQueue(ID3D12CommandQueue* queue);

    bool attach(IDXGISwapChain* swapChain);

    void releaseTargets();

    void shutdown();

    bool beginFrame();

    void endFrame();

    [[nodiscard]] ID2D1DeviceContext* d2d() const { return d2dContext_.get(); }
    [[nodiscard]] IDWriteFactory5* dwrite() const { return dwriteFactory_.get(); }
    [[nodiscard]] ID2D1Factory1* d2dFactory() const { return d2dFactory_.get(); }
    [[nodiscard]] Vec2 size() const { return size_; }
    [[nodiscard]] Backend backend() const { return backend_; }
    [[nodiscard]] bool ready() const { return ready_; }
    [[nodiscard]] HWND window() const { return window_; }

    [[nodiscard]] bool deviceLost() const { return deviceLost_; }
    void markDeviceLost();

private:
    GraphicsContext() = default;
    ~GraphicsContext() = default;

    bool createDeviceResources(IDXGISwapChain* swapChain);
    bool createTargets(IDXGISwapChain* swapChain);
    bool createD2D(IUnknown* deviceForD2D);

    std::recursive_mutex mutex_;

    IDXGISwapChain* swapChain_ = nullptr;
    ComPtr<IDXGISwapChain3> swapChain3_;
    ComPtr<ID3D12CommandQueue> commandQueue_;

    ComPtr<ID3D12Device> device12_;
    ComPtr<ID3D11Device> device11_;
    ComPtr<ID3D11DeviceContext> context11_;
    ComPtr<ID3D11On12Device> device11On12_;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory5> dwriteFactory_;

    struct FrameTarget {
        ComPtr<ID3D11Resource> wrapped;
        ComPtr<ID2D1Bitmap1> bitmap;
    };
    std::vector<FrameTarget> targets_;

    HWND window_ = nullptr;
    Vec2 size_;
    Backend backend_ = Backend::None;
    unsigned currentBuffer_ = 0;
    bool ready_ = false;
    bool drawing_ = false;
    bool deviceLost_ = false;
    bool warnedNoQueue_ = false;
};

}
