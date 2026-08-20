#include "GraphicsContext.hpp"

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
    Log::debug(kLog, "file de commandes D3D12 capturée");
}

void GraphicsContext::markDeviceLost() {
    const std::lock_guard lock(mutex_);
    if (deviceLost_) return;

    deviceLost_ = true;
    ready_ = false;
    Log::warn(kLog, "périphérique perdu : ressources graphiques libérées");

    releaseTargets();

    DeviceLostEvent event;
    events().emit(event);
}

bool GraphicsContext::attach(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;

    const std::lock_guard lock(mutex_);

    if (ready_ && swapChain_ == swapChain && !deviceLost_) return true;

    if (swapChain_ != swapChain || deviceLost_) {
        shutdown();
        deviceLost_ = false;
    }

    swapChain_ = swapChain;

    if (!createDeviceResources(swapChain)) return false;
    if (!createTargets(swapChain)) return false;

    ready_ = true;
    Log::info(kLog, "overlay attaché (backend {}, {}x{})",
              backend_ == Backend::D3D12 ? "D3D12/11on12" : "D3D11",
              static_cast<int>(size_.x), static_cast<int>(size_.y));
    return true;
}

bool GraphicsContext::createDeviceResources(IDXGISwapChain* swapChain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) {
        Log::error(kLog, "IDXGISwapChain::GetDesc a échoué");
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
                Log::debug(kLog, "en attente de la file de commandes D3D12 du jeu");
                warnedNoQueue_ = true;
            }
            return false;
        }

        // La file doit etre celle du jeu : en creer une bloque au present.
        IUnknown* queues[] = {commandQueue_.get()};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef VELYX_DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        const HRESULT hr = D3D11On12CreateDevice(
            device12_.get(), flags, nullptr, 0, queues, 1, 0,
            device11_.put(), context11_.put(), nullptr);
        if (FAILED(hr)) {
            Log::error(kLog, "D3D11On12CreateDevice a échoué (0x{:08X})", static_cast<unsigned>(hr));
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
        Log::error(kLog, "D2D1CreateFactory a échoué (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5),
                             reinterpret_cast<IUnknown**>(dwriteFactory_.put()));
    if (FAILED(hr)) {
        Log::error(kLog, "DWriteCreateFactory a échoué (0x{:08X})", static_cast<unsigned>(hr));
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
        Log::error(kLog, "ID2D1Factory1::CreateDevice a échoué (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext_.put());
    if (FAILED(hr)) {
        Log::error(kLog, "CreateDeviceContext a échoué (0x{:08X})", static_cast<unsigned>(hr));
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
                Log::error(kLog, "GetBuffer({}) a échoué", i);
                return false;
            }

            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;

            const HRESULT hr = device11On12_->CreateWrappedResource(
                backBuffer.get(), &flags, D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT, __uuidof(ID3D11Resource),
                reinterpret_cast<void**>(targets_[i].wrapped.put()));
            if (FAILED(hr)) {
                Log::error(kLog, "CreateWrappedResource({}) a échoué (0x{:08X})", i,
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
                Log::error(kLog, "GetBuffer(0) en IDXGISurface a échoué");
                return false;
            }
        }

        const HRESULT hr = d2dContext_->CreateBitmapFromDxgiSurface(
            surface.get(), &properties, targets_[i].bitmap.put());
        if (FAILED(hr)) {
            Log::error(kLog, "CreateBitmapFromDxgiSurface({}) a échoué (0x{:08X})", i,
                       static_cast<unsigned>(hr));
            return false;
        }
    }

    size_ = {static_cast<float>(desc.BufferDesc.Width), static_cast<float>(desc.BufferDesc.Height)};
    return true;
}

// Doit tourner avant le ResizeBuffers du jeu : DXGI refuse le redimensionnement
// tant que nos ressources enveloppees tiennent une reference sur les back buffers.
void GraphicsContext::releaseTargets() {
    const std::lock_guard lock(mutex_);

    if (drawing_ && d2dContext_) {
        d2dContext_->EndDraw();
        drawing_ = false;
    }

    if (d2dContext_) d2dContext_->SetTarget(nullptr);

    targets_.clear();

    if (context11_) {
        context11_->ClearState();
        context11_->Flush();
    }

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
}

bool GraphicsContext::beginFrame() {
    const std::lock_guard lock(mutex_);

    if (!ready_ || drawing_ || targets_.empty() || !d2dContext_) return false;

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
    return true;
}

void GraphicsContext::endFrame() {
    const std::lock_guard lock(mutex_);

    if (!drawing_) return;
    drawing_ = false;

    const HRESULT hr = d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);

    if (backend_ == Backend::D3D12 && currentBuffer_ < targets_.size()) {
        ID3D11Resource* wrapped = targets_[currentBuffer_].wrapped.get();
        device11On12_->ReleaseWrappedResources(&wrapped, 1);

        // Sans ce flush le travail 11on12 n'est jamais soumis et l'overlay reste invisible.
        context11_->Flush();
    }

    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_REMOVED) ||
        hr == static_cast<HRESULT>(DXGI_ERROR_DEVICE_RESET)) {
        markDeviceLost();
    } else if (FAILED(hr)) {
        Log::warn(kLog, "EndDraw a échoué (0x{:08X})", static_cast<unsigned>(hr));
    }
}

}
