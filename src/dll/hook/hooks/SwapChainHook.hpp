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

    // What the game asks Present for, and what it gets. Bedrock picks its own sync
    // interval from the graphics menu, and this is the only place that can overrule
    // it without writing to the game's memory.
    struct Presentation {
        bool bypassVsync = false;
        bool allowTearing = true;
    };
    static void setPresentation(Presentation presentation);

    // Tearing needs a flag that can only be set when the swapchain is created, and
    // asking for it without that flag makes Present refuse the frame outright. So the
    // answer is read off the swapchain the game actually built, never assumed, and
    // forgotten again on a resize in case the window went fullscreen in between.
    // Unknown until the first frame has been presented, or until probeTearing() has
    // asked on the spot — which is what a setting being changed needs, so that the
    // answer can be reported at the moment it is asked for rather than a frame later.
    static bool tearingAvailable();
    static void probeTearing();

    // The swapchain the game last presented, for anything that has a question to ask
    // it rather than something to draw on it. Null until the first frame.
    static IDXGISwapChain* lastPresented();

private:

    static bool captureVTables(void** swapChainVTable, size_t swapChainCount,
                               void** queueVTable, size_t queueCount);

    void* presentTarget_ = nullptr;
    void* present1Target_ = nullptr;
    void* resizeTarget_ = nullptr;
    void* executeTarget_ = nullptr;
};

}
