#pragma once

#include <string_view>

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <filesystem>

#include "dll/event/Events.hpp"
#include "dll/render/Renderer.hpp"

namespace velyx {

class Velyx {
public:
    static Velyx& get();

    void start(HMODULE self);

    void requestEject();

    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] bool ready() const { return ready_.load(std::memory_order_acquire); }
    [[nodiscard]] HMODULE moduleHandle() const { return self_; }

    [[nodiscard]] Renderer& renderer() { return renderer_; }
    [[nodiscard]] float delta() const { return delta_; }
    [[nodiscard]] uint64_t frameIndex() const { return frameIndex_; }
    [[nodiscard]] Vec2 screenSize() const { return screenSize_; }

    [[nodiscard]] float fps() const { return fps_; }

    [[nodiscard]] double uptime() const;

    // Where a bundled file lives for this session: next to the DLL when it was
    // injected from a build tree, in %APPDATA%\Velyx when the launcher unpacked it.
    [[nodiscard]] std::filesystem::path asset(std::string_view relative) const;

    [[nodiscard]] bool screenshotMode() const { return screenshotMode_; }
    void setScreenshotMode(bool enabled) { screenshotMode_ = enabled; }

private:
    Velyx() = default;

    void bootstrap();
    void initialiseGraphicsOnce();
    void onPresent(IDXGISwapChain* swapChain);
    void onSwapchainRebuilt(unsigned width, unsigned height);
    void presentFrame(IDXGISwapChain* swapChain);
    void onRenderFailure(std::string_view reason);

    void onDeviceLost(DeviceLostEvent& event);
    void onGameClosing(GameClosingEvent& event);
    void onWindowResized(SwapchainResizeEvent& event);
    void onWindowDrag(WindowDragEvent& event);
    void shutdown();

    HMODULE self_ = nullptr;
    HANDLE thread_ = nullptr;
    std::filesystem::path assets_;

    Renderer renderer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> ejecting_{false};
    std::atomic<bool> closing_{false};
    int renderFailures_ = 0;

    bool graphicsInitialised_ = false;
    bool screenshotMode_ = false;

    float delta_ = 0.f;
    float fps_ = 0.f;
    uint64_t frameIndex_ = 0;
    Vec2 screenSize_;

    long long lastFrameTicks_ = 0;
    long long startTicks_ = 0;
    long long ticksPerSecond_ = 0;
};

}
