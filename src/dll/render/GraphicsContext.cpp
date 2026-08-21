#include "GraphicsContext.hpp"

#include <utility>

#include "core/Log.hpp"
#include "dll/event/Events.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Graphics";

DXGI_FORMAT toD2DFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:

            return DXGI_FORMAT_B8G8R8A8_UNORM;
        default:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

}

GraphicsContext& GraphicsContext::get() {
    static GraphicsContext instance;
    return instance;
}

void GraphicsContext::setCommandQueue(ID3D12CommandQueue* queue) {
    if (!queue) return;

    const std::lock_guard lock(mutex_);
    if (commandQueue_.get() == queue) return;

    const D3D12_COMMAND_QUEUE_DESC desc = queue->GetDesc();
    if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;

    commandQueue_ = ComPtr<ID3D12CommandQueue>(queue);
    Log::debug(kLog, "captured the D3D12 direct command queue");
}

void GraphicsContext::markDeviceLost() {
    const std::lock_guard lock(mutex_);
    markDeviceLostLocked("device lost", Sync::Full);
}

// Callers that already hold the lock. The reason is logged rather than assumed: this
// is now reached from a window resize as well as from a genuinely lost device.
void GraphicsContext::markDeviceLostLocked(std::string_view reason, Sync sync) {
    if (deviceLost_) return;

    deviceLost_ = true;
    ready_ = false;
    Log::warn(kLog, "{}, dropping graphics resources", reason);

    releaseTargets(sync);
    Log::info(kLog, "release: targets gone");

    DeviceLostEvent event;
    events().emit(event);

    Log::info(kLog, "release: done");
}

// How long the window has to hold still before the overlay trusts its size again.
// A drag delivers a WM_SIZE every few dozen milliseconds, so anything shorter puts
// the rebuild back inside the drag.
constexpr uint64_t kSettleMs = 250;

// The description and the client rect do not always agree — vkd3d-proton reallocates
// its swapchain on its own schedule — and waiting forever for them to would leave the
// overlay dark. Past this, the window has been quiet long enough that the buffers are
// safe whatever the description says.
constexpr uint64_t kForceMs = 1500;

bool GraphicsContext::attach(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;

    applyPendingRelease();

    const std::lock_guard lock(mutex_);

    // The client rect is the one size that is true on both stacks. DXGI's description
    // does not move when vkd3d-proton reallocates underneath, and the game never calls
    // ResizeBuffers there, so nothing else reports the change — and drawing into
    // buffers that no longer describe the window is what takes the process down.
    HWND window = window_;
    if (!window) {
        DXGI_SWAP_CHAIN_DESC desc{};
        if (SUCCEEDED(swapChain->GetDesc(&desc))) window = desc.OutputWindow;
    }

    RECT client{};
    unsigned clientWidth = 0;
    unsigned clientHeight = 0;
    if (window && GetClientRect(window, &client)) {
        clientWidth = static_cast<unsigned>(client.right - client.left);
        clientHeight = static_cast<unsigned>(client.bottom - client.top);
    }

    if (clientWidth != lastClientWidth_ || clientHeight != lastClientHeight_) {
        lastClientWidth_ = clientWidth;
        lastClientHeight_ = clientHeight;
        noteWindowChanged();

        // Letting go of the buffers is not enough. The 11on12 and D2D devices are built
        // on the game's own device and its command queue, and vkd3d-proton reallocates
        // the swapchain from a thread of its own — the game ends itself somewhere in
        // there, with nothing of ours on the stack. So everything interop goes, not
        // just the buffers, and the next settled frame builds it back.
        if (!targets_.empty() || d2dContext_) {
            Log::debug(kLog, "window is now {}x{}, going dark", clientWidth, clientHeight);
            markDeviceLostLocked("window resized", Sync::None);
        }
        return false;
    }

    // Minimised: there is nothing to draw on and no size to build for.
    if (clientWidth == 0 || clientHeight == 0) return false;

    if (ready_ && swapChain_ == swapChain && !deviceLost_ && !targets_.empty()) return true;

    // Everything past here rebuilds GPU resources from the swapchain, so it may only
    // run once the window has finished moving. Rebuilding mid-drag is what the crash
    // on a large resize was: the buffers were handed over, then replaced underneath
    // before the first frame reached them.
    if (windowMoving_.load(std::memory_order_acquire)) return false;

    const uint64_t quiet = GetTickCount64() - lastChangeMs_.load(std::memory_order_acquire);
    if (quiet < kSettleMs) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;

    const bool agrees =
        desc.BufferDesc.Width == clientWidth && desc.BufferDesc.Height == clientHeight;
    if (!agrees && quiet < kForceMs) return false;

    // A different swapchain does not mean a different device: the game recreates its
    // swapchain on every window change, but the device behind it lives on. Only the
    // buffers are new, so only the targets are rebuilt — tearing the 11on12 and D2D
    // devices down and back up seven times in nine seconds is churn the process does
    // not need and did not always survive.
    bool sameDevice = false;
    if (!deviceLost_ && d2dContext_) {
        if (backend_ == Backend::D3D12 && device12_) {
            ComPtr<ID3D12Device> device;
            sameDevice = SUCCEEDED(swapChain->GetDevice(
                             __uuidof(ID3D12Device), reinterpret_cast<void**>(device.put()))) &&
                         device.get() == device12_.get();
        } else if (backend_ == Backend::D3D11 && device11_) {
            ComPtr<ID3D11Device> device;
            sameDevice = SUCCEEDED(swapChain->GetDevice(
                             __uuidof(ID3D11Device), reinterpret_cast<void**>(device.put()))) &&
                         device.get() == device11_.get();
        }
    }

    Log::debug(kLog, "rebuild: client {}x{}, buffers {}x{} x{}, quiet {} ms, {} device",
               clientWidth, clientHeight, desc.BufferDesc.Width, desc.BufferDesc.Height,
               desc.BufferCount, quiet, sameDevice ? "same" : "new");

    releaseTargets();

    if (sameDevice) {
        swapChain_ = swapChain;
        swapChain3_.reset();
        swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                                  reinterpret_cast<void**>(swapChain3_.put()));
        window_ = desc.OutputWindow;
    } else {
        // A different device, or none yet: everything has to go.
        if (swapChain_ != nullptr || deviceLost_) {
            Log::info(kLog, "rebuild: dropping the interop devices");
            shutdown();
            deviceLost_ = false;
            Log::info(kLog, "rebuild: interop devices dropped");
        }

        swapChain_ = swapChain;
        if (!createDeviceResources(swapChain)) return false;
    }

    if (!createTargets(swapChain)) {
        Log::warn(kLog, "rebuild: createTargets failed");
        return false;
    }

    // createTargets sizes itself from the description; where the window disagrees, the
    // window is the one the client lays out against.
    if (agrees) {
        size_ = {static_cast<float>(clientWidth), static_cast<float>(clientHeight)};
    }

    ready_ = true;
    resized_ = true;

    if (sameDevice) {
        Log::info(kLog, "overlay rebuilt for {}x{}", static_cast<int>(size_.x),
                  static_cast<int>(size_.y));
    } else {
        Log::info(kLog, "overlay attached ({} backend, {}x{})",
                  backend_ == Backend::D3D12 ? "D3D12/11on12" : "D3D11",
                  static_cast<int>(size_.x), static_cast<int>(size_.y));
    }
    return true;
}

void GraphicsContext::noteWindowChanged() {
    lastChangeMs_.store(GetTickCount64(), std::memory_order_release);
}

void GraphicsContext::setWindowMoving(bool moving) {
    windowMoving_.store(moving, std::memory_order_release);
    noteWindowChanged();
}

// Called from the window procedure, so it must not do any of the work: waiting on a
// fence here froze the game's message pump — and, through the context lock, the render
// thread with it — in the middle of a resize. It only leaves a note.
void GraphicsContext::releaseForResize() {
    noteWindowChanged();
    releasePending_.store(true, std::memory_order_release);
}

// The render thread's side of that note, run before anything touches the targets.
void GraphicsContext::applyPendingRelease() {
    if (!releasePending_.exchange(false, std::memory_order_acq_rel)) return;

    const std::lock_guard lock(mutex_);
    if (targets_.empty() && !d2dContext_) {
        ready_ = false;
        return;
    }

    markDeviceLostLocked("window changed", Sync::None);
}

bool GraphicsContext::waitForGpu() {
    if (!device12_ || !commandQueue_) return false;

    ComPtr<ID3D12Fence> fence;
    if (FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                      reinterpret_cast<void**>(fence.put())))) {
        return false;
    }

    const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!done) return false;

    bool waited = false;
    if (SUCCEEDED(commandQueue_->Signal(fence.get(), 1))) {
        if (fence->GetCompletedValue() < 1 &&
            SUCCEEDED(fence->SetEventOnCompletion(1, done))) {
            waited = WaitForSingleObject(done, 200) == WAIT_OBJECT_0;
        } else {
            waited = true;
        }
    }

    CloseHandle(done);
    return waited;
}

bool GraphicsContext::takeResized() {
    const std::lock_guard lock(mutex_);
    return std::exchange(resized_, false);
}

bool GraphicsContext::createDeviceResources(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) {
        Log::error(kLog, "IDXGISwapChain::GetDesc failed");
        return false;
    }

    window_ = desc.OutputWindow;
    size_ = {static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height)};

    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device),
                                       reinterpret_cast<void**>(device12_.put())))) {
        backend_ = Backend::D3D12;
    } else if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device),
                                              reinterpret_cast<void**>(device11_.put())))) {
        backend_ = Backend::D3D11;
        device11_->GetImmediateContext(context11_.put());
    } else {
        Log::error(kLog, "swapchain is backed by neither D3D11 nor D3D12");
        return false;
    }

    if (backend_ == Backend::D3D12) {
        if (!commandQueue_) {

            if (!warnedNoQueue_) {
                Log::debug(kLog, "waiting for the game's D3D12 command queue");
                warnedNoQueue_ = true;
            }
            return false;
        }

        // The queue must be the game's own: creating one deadlocks on present.
        IUnknown* queues[] = {commandQueue_.get()};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef VELYX_DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const HRESULT hr = D3D11On12CreateDevice(
            device12_.get(), flags, nullptr, 0, queues, 1, 0,
            device11_.put(), context11_.put(), nullptr);
        if (FAILED(hr)) {
            Log::error(kLog, "D3D11On12CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }

        if (FAILED(device11_->QueryInterface(__uuidof(ID3D11On12Device),
                                             reinterpret_cast<void**>(device11On12_.put())))) {
            Log::error(kLog, "ID3D11On12Device indisponible");
            return false;
        }
    }

    swapChain->QueryInterface(__uuidof(IDXGISwapChain3),
                              reinterpret_cast<void**>(swapChain3_.put()));

    return createD2D(device11_.get());
}

bool GraphicsContext::createD2D(IUnknown* deviceForD2D) {
    D2D1_FACTORY_OPTIONS options{};
#ifdef VELYX_DEBUG
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                                   &options, reinterpret_cast<void**>(d2dFactory_.put()));
    if (FAILED(hr)) {
        Log::error(kLog, "D2D1CreateFactory failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                             reinterpret_cast<IUnknown**>(dwriteFactory_.put()));
    if (FAILED(hr)) {
        Log::error(kLog, "DWriteCreateFactory failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = deviceForD2D->QueryInterface(__uuidof(IDXGIDevice),
                                      reinterpret_cast<void**>(dxgiDevice.put()));
    if (FAILED(hr)) {
        Log::error(kLog, "IDXGIDevice indisponible (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = d2dFactory_->CreateDevice(dxgiDevice.get(), d2dDevice_.put());
    if (FAILED(hr)) {
        Log::error(kLog, "ID2D1Factory1::CreateDevice failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext_.put());
    if (FAILED(hr)) {
        Log::error(kLog, "CreateDeviceContext failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    d2dContext_->SetDpi(96.f, 96.f);
    d2dContext_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    d2dContext_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    return true;
}

bool GraphicsContext::createTargets(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return false;

    const UINT bufferCount = backend_ == Backend::D3D12 ? desc.BufferCount : 1;
    targets_.clear();
    targets_.resize(bufferCount);
    wrappedFrom_.assign(bufferCount, nullptr);

    const D2D1_BITMAP_PROPERTIES1 properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(toD2DFormat(desc.BufferDesc.Format), D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);

    for (UINT i = 0; i < bufferCount; ++i) {
        ComPtr<IDXGISurface> surface;

        if (backend_ == Backend::D3D12) {
            ComPtr<ID3D12Resource> backBuffer;
            if (FAILED(swapChain->GetBuffer(i, __uuidof(ID3D12Resource),
                                            reinterpret_cast<void**>(backBuffer.put())))) {
                Log::error(kLog, "GetBuffer({}) failed", i);
                return false;
            }

            wrappedFrom_[i] = backBuffer.get();

            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;

            const HRESULT hr = device11On12_->CreateWrappedResource(
                backBuffer.get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource),
                reinterpret_cast<void**>(targets_[i].wrapped.put()));
            if (FAILED(hr)) {
                Log::error(kLog, "CreateWrappedResource({}) failed (0x{:08X})", i,
                           static_cast<unsigned>(hr));
                return false;
            }

            if (FAILED(targets_[i].wrapped->QueryInterface(
                    __uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put())))) {
                Log::error(kLog, "wrapped resource is not an IDXGISurface");
                return false;
            }
        } else {
            if (FAILED(swapChain->GetBuffer(0, __uuidof(IDXGISurface),
                                            reinterpret_cast<void**>(surface.put())))) {
                Log::error(kLog, "GetBuffer(0) as IDXGISurface failed");
                return false;
            }
        }

        const HRESULT hr = d2dContext_->CreateBitmapFromDxgiSurface(
            surface.get(), &properties, targets_[i].bitmap.put());
        if (FAILED(hr)) {
            Log::error(kLog, "CreateBitmapFromDxgiSurface({}) failed (0x{:08X})", i,
                       static_cast<unsigned>(hr));
            return false;
        }
    }

    size_ = {static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height)};
    return true;
}

// Must run before the game's ResizeBuffers: DXGI refuses to resize while our
// wrapped resources still hold references on the back buffers.
void GraphicsContext::releaseTargets(Sync sync) {
    const std::lock_guard lock(mutex_);

    if (drawing_) {
        Log::debug(kLog, "releaseTargets: a frame was still open");
        drawing_ = false;
        if (d2dContext_) d2dContext_->EndDraw();

        // A wrapped back buffer has to go back to 11on12 before it is destroyed.
        // ResizeBuffers lands mid-frame when the game goes fullscreen, and dropping
        // a still-acquired resource there takes the whole process down.
        if (backend_ == Backend::D3D12 && device11On12_ && currentBuffer_ < targets_.size()) {
            ID3D11Resource* wrapped = targets_[currentBuffer_].wrapped.get();
            device11On12_->ReleaseWrappedResources(&wrapped, 1);
        }
    }

    if (d2dContext_) d2dContext_->SetTarget(nullptr);

    // Unbind and submit before anything is destroyed, so the flush never carries a
    // command stream naming resources that are already gone. Then let the GPU finish
    // with what was just submitted: a wrapped back buffer must not be freed while it
    // is still being read.
    //
    // Both talk to the game's queue, and during a resize that is exactly what must not
    // happen — the buffers were handed back at the end of the last frame, and the game
    // synchronises its own queue to recreate a swapchain anyway.
    if (sync == Sync::Full) {
        if (context11_) {
            context11_->ClearState();
            context11_->Flush();
        }
        waitForGpu();
    }

    targets_.clear();
    wrappedFrom_.clear();

    ready_ = false;
}

void GraphicsContext::shutdown() {
    const std::lock_guard lock(mutex_);

    releaseTargets();

    d2dContext_.reset();
    d2dDevice_.reset();
    d2dFactory_.reset();
    dwriteFactory_.reset();

    device11On12_.reset();
    context11_.reset();
    device11_.reset();
    device12_.reset();
    swapChain3_.reset();
    commandQueue_.reset();

    swapChain_ = nullptr;
    backend_ = Backend::None;
    ready_ = false;
    wrappedFrom_.clear();
    lastClientWidth_ = 0;
    lastClientHeight_ = 0;
}

bool GraphicsContext::beginFrame() {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    if (!ready_ || drawing_ || targets_.empty() || !d2dContext_) return false;

    // The window procedure may have asked for a release since attach() ran. Starting a
    // frame on buffers already spoken for is the race the whole settle dance exists to
    // avoid, and the note costs one atomic read to honour.
    if (releasePending_.load(std::memory_order_acquire)) return false;

    currentBuffer_ = 0;
    if (backend_ == Backend::D3D12) {
        if (swapChain3_) currentBuffer_ = swapChain3_->GetCurrentBackBufferIndex();
        if (currentBuffer_ >= targets_.size()) return false;

        ID3D11Resource* wrapped = targets_[currentBuffer_].wrapped.get();
        device11On12_->AcquireWrappedResources(&wrapped, 1);
    }

    d2dContext_->SetTarget(targets_[currentBuffer_].bitmap.get());
    d2dContext_->BeginDraw();
    d2dContext_->SetTransform(D2D1::Matrix3x2F::Identity());

    drawing_ = true;

    // The draw calls between here and endFrame() run without the lock otherwise,
    // while ResizeBuffers arrives on another thread and frees the very targets they
    // are writing to — going fullscreen with the menu open is what hit it. Holding
    // the frame's lock makes that resize wait for the frame instead.
    frameLock_ = std::move(lock);
    return true;
}

void GraphicsContext::endFrame() {
    const std::lock_guard lock(mutex_);

    if (!drawing_) {
        frameLock_ = {};
        return;
    }
    drawing_ = false;

    const HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);

    if (backend_ == Backend::D3D12 && currentBuffer_ < targets_.size()) {
        ID3D11Resource* wrapped = targets_[currentBuffer_].wrapped.get();
        device11On12_->ReleaseWrappedResources(&wrapped, 1);

        // Without this flush the 11on12 work is never submitted and the overlay
        // never appears.
        context11_->Flush();
    }

    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        markDeviceLost();
    } else if (FAILED(hr)) {
        Log::warn(kLog, "EndDraw failed (0x{:08X})", static_cast<unsigned>(hr));
    }

    frameLock_ = {};
}

}
