#pragma once

#include <dxgi1_4.h>
#include <d3d12.h>

#include <functional>

#include "dll/hook/Hook.hpp"

namespace velyx {

class SwapChainHook final : public Hook {
public:
    SwapChainHook();

    bool install() override;
    void uninstall() override;

    using PresentCallback = std::function<void(IDXGISwapChain*)>;
    static void setPresentCallback(PresentCallback callback);

    static bool presenting();

private:

    static bool captureVTables(void** swapChainVTable, size_t swapChainCount,
                               void** queueVTable, size_t queueCount);

    void* presentTarget_ = nullptr;
    void* present1Target_ = nullptr;
    void* resizeTarget_ = nullptr;
    void* executeTarget_ = nullptr;
};

}
