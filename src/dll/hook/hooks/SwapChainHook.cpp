#include "SwapChainHook.hpp"

#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstring>

#include <MinHook.h>

#include "core/Log.hpp"
#include "dll/render/ComPtr.hpp"
#include "dll/render/GraphicsContext.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "SwapChain";

// Emplacements figes par l'ABI COM, inchanges depuis DXGI 1.2.
constexpr size_t kPresentIndex = 8;
constexpr size_t kResizeBuffersIndex = 13;
constexpr size_t kPresent1Index = 22;
constexpr size_t kSwapChainMethodCount = 40;

constexpr size_t kExecuteCommandListsIndex = 10;
constexpr size_t kQueueMethodCount = 20;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT,
                                                       ID3D12CommandList* const*);

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;

SwapChainHook::PresentCallback g_onPresent;
SwapChainHook::ResizeCallback g_onResize;
std::atomic<bool> g_presenting{false};

void dispatchPresent(IDXGISwapChain* swapChain) {
    g_presenting.store(true, std::memory_order_relaxed);
    if (g_onPresent) g_onPresent(swapChain);
}

HRESULT STDMETHODCALLTYPE presentDetour(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {

    if (!(flags & DXGI_PRESENT_TEST)) dispatchPresent(swapChain);
    return g_originalPresent(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE present1Detour(IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* parameters) {
    if (!(flags & DXGI_PRESENT_TEST)) dispatchPresent(swapChain);
    return g_originalPresent1(swapChain, syncInterval, flags, parameters);
}

HRESULT STDMETHODCALLTYPE resizeBuffersDetour(IDXGISwapChain* swapChain, UINT bufferCount,
                                              UINT width, UINT height, DXGI_FORMAT format,
                                              UINT flags) {

    GraphicsContext::get().releaseTargets();
    if (g_onResize) g_onResize(width, height);

    return g_originalResizeBuffers(swapChain, bufferCount, width, height, format, flags);
}

void STDMETHODCALLTYPE executeCommandListsDetour(ID3D12CommandQueue* queue, UINT count,
                                                 ID3D12CommandList* const* lists) {
    GraphicsContext::get().setCommandQueue(queue);
    g_originalExecuteCommandLists(queue, count, lists);
}

class DummyWindow {
public:
    DummyWindow() {
        windowClass_.cbSize = sizeof(WNDCLASSEXW);
        windowClass_.style = CS_HREDRAW | CS_VREDRAW;
        windowClass_.lpfnWndProc = DefWindowProcW;
        windowClass_.hInstance = GetModuleHandleW(nullptr);
        windowClass_.lpszClassName = L"VelyxProbe";

        RegisterClassExW(&windowClass_);
        handle_ = CreateWindowW(windowClass_.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                                nullptr, nullptr, windowClass_.hInstance, nullptr);
    }

    ~DummyWindow() {
        if (handle_) DestroyWindow(handle_);
        UnregisterClassW(windowClass_.lpszClassName, windowClass_.hInstance);
    }

    [[nodiscard]] HWND handle() const { return handle_; }

private:
    WNDCLASSEXW windowClass_{};
    HWND handle_ = nullptr;
};

}

SwapChainHook::SwapChainHook() : Hook("SwapChain", 0) {}

bool SwapChainHook::presenting() { return g_presenting.load(std::memory_order_relaxed); }

void SwapChainHook::setPresentCallback(PresentCallback callback) {
    g_onPresent = std::move(callback);
}

void SwapChainHook::setResizeCallback(ResizeCallback callback) {
    g_onResize = std::move(callback);
}

bool SwapChainHook::captureVTables(void** swapChainVTable, size_t swapChainCount,
                                   void** queueVTable, size_t queueCount) {
    const DummyWindow window;
    if (!window.handle()) {
        Log::error(kLog, "fenêtre témoin impossible à créer");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.BufferCount = 2;
    desc.Width = 1;
    desc.Height = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(factory.put())))) {
        Log::error(kLog, "CreateDXGIFactory1 a échoué");
        return false;
    }

    ComPtr<ID3D12Device> device12;
    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                                    reinterpret_cast<void**>(device12.put())))) {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

        ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(device12->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
                                                   reinterpret_cast<void**>(queue.put())))) {
            ComPtr<IDXGISwapChain1> swapChain;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue.get(), window.handle(), &desc,
                                                          nullptr, nullptr, swapChain.put()))) {
                std::memcpy(swapChainVTable, *reinterpret_cast<void***>(swapChain.get()),
                            swapChainCount * sizeof(void*));
                std::memcpy(queueVTable, *reinterpret_cast<void***>(queue.get()),
                            queueCount * sizeof(void*));

                Log::debug(kLog, "vtables lues depuis une swapchain témoin D3D12");
                return true;
            }
        }
    }

    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;

    if (SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                    D3D11_SDK_VERSION, device11.put(), &level, context11.put()))) {
        ComPtr<IDXGISwapChain1> swapChain;
        if (SUCCEEDED(factory->CreateSwapChainForHwnd(device11.get(), window.handle(), &desc,
                                                      nullptr, nullptr, swapChain.put()))) {
            std::memcpy(swapChainVTable, *reinterpret_cast<void***>(swapChain.get()),
                        swapChainCount * sizeof(void*));
            std::memset(queueVTable, 0, queueCount * sizeof(void*));

            Log::debug(kLog, "vtables lues depuis une swapchain témoin D3D11");
            return true;
        }
    }

    Log::error(kLog, "aucun périphérique graphique utilisable : overlay impossible");
    return false;
}

bool SwapChainHook::install() {
    void* swapChainVTable[kSwapChainMethodCount]{};
    void* queueVTable[kQueueMethodCount]{};

    if (!captureVTables(swapChainVTable, kSwapChainMethodCount, queueVTable, kQueueMethodCount)) {
        return false;
    }

    presentTarget_ = swapChainVTable[kPresentIndex];
    present1Target_ = swapChainVTable[kPresent1Index];
    resizeTarget_ = swapChainVTable[kResizeBuffersIndex];
    executeTarget_ = queueVTable[kExecuteCommandListsIndex];

    bool ok = createAt(presentTarget_, reinterpret_cast<void*>(&presentDetour),
                       reinterpret_cast<void**>(&g_originalPresent));

    if (present1Target_) {
        Hook::createAt(present1Target_, reinterpret_cast<void*>(&present1Detour),
                       reinterpret_cast<void**>(&g_originalPresent1));
    }

    if (resizeTarget_) {
        Hook::createAt(resizeTarget_, reinterpret_cast<void*>(&resizeBuffersDetour),
                       reinterpret_cast<void**>(&g_originalResizeBuffers));
    }

    if (executeTarget_) {
        Hook::createAt(executeTarget_, reinterpret_cast<void*>(&executeCommandListsDetour),
                       reinterpret_cast<void**>(&g_originalExecuteCommandLists));
    }

    installed_ = ok;
    return ok;
}

void SwapChainHook::uninstall() {
    for (void* target : {presentTarget_, present1Target_, resizeTarget_, executeTarget_}) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
    }

    presentTarget_ = present1Target_ = resizeTarget_ = executeTarget_ = nullptr;
    installed_ = false;
    g_presenting.store(false, std::memory_order_relaxed);
}

}
