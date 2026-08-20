#pragma once

#include <windows.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>

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

    [[nodiscard]] bool screenshotMode() const { return screenshotMode_; }
    void setScreenshotMode(bool enabled) { screenshotMode_ = enabled; }

private:
    Velyx() = default;

    void bootstrap();
    void initialiseGraphicsOnce();
    void onPresent(IDXGISwapChain* swapChain);
    void onResize(unsigned width, unsigned height);
    void onDeviceLost(DeviceLostEvent& event);
    void shutdown();

    HMODULE self_ = nullptr;
    HANDLE thread_ = nullptr;

    Renderer renderer_;

    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> ejecting_{false};

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
