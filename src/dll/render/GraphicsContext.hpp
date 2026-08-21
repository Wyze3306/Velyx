#pragma once

#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dwrite_3.h>
#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <string_view>
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

    // True once after the swapchain changed size without anyone announcing it.
    [[nodiscard]] bool takeResized();

    // Drops every reference to the current back buffers, at once. The window changing
    // size means the game is about to destroy its swapchain, and it must not find our
    // references still on its buffers when it does.
    void releaseForResize();

    // Set around an interactive border drag: nothing is rebuilt while it holds.
    void setWindowMoving(bool moving);

    // Runs the release that releaseForResize() asked for, on the render thread.
    void applyPendingRelease();

    // Whether letting go may touch the game's own command queue. A resize may not:
    // vkd3d-proton is recreating the swapchain from its present task while this runs,
    // and a fence signalled on that queue from here is the last thing in the log.
    enum class Sync { None, Full };
    void releaseTargets(Sync sync = Sync::Full);

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
    void markDeviceLostLocked(std::string_view reason, Sync sync);
    bool createD2D(IUnknown* deviceForD2D);

    std::recursive_mutex mutex_;

    // Set when attach() rebuilt the targets for a size it was never told about, so
    // the client can relayout for it. Cleared by takeResized().
    bool resized_ = false;

    // Held from beginFrame() to endFrame(): see the note in beginFrame().
    std::unique_lock<std::recursive_mutex> frameLock_;

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

    // The back buffer each target was built on, kept only to notice when the swapchain
    // hands out different ones: see attach().
    std::vector<void*> wrappedFrom_;

    // The client size the last frame saw. Any disagreement with it means the buffers
    // we hold no longer describe the window, and the overlay stops drawing at once.
    unsigned lastClientWidth_ = 0;
    unsigned lastClientHeight_ = 0;

    // When the window last changed, in milliseconds. Rebuilding is gated on this
    // rather than on a frame count: frames only tick while the game presents, so a
    // handful of them can pass inside a single drag. See attach().
    std::atomic<uint64_t> lastChangeMs_{0};
    std::atomic<bool> windowMoving_{false};

    // Set from the window procedure, acted on by the render thread: see
    // releaseForResize().
    std::atomic<bool> releasePending_{false};

    // Stamps lastChangeMs_. Called from whichever thread noticed the change.
    void noteWindowChanged();

    // Blocks until the queue has finished with what it was given, so wrapped back
    // buffers are never freed while the GPU is still reading them.
    bool waitForGpu();

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
