#include "Velyx.hpp"

#include <velyx/Version.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "dll/config/ClientConfig.hpp"
#include "dll/config/ProfileManager.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/feature/Playtime.hpp"
#include "dll/feature/Services.hpp"
#include "dll/feature/Updates.hpp"
#include "dll/hook/HookManager.hpp"
#include "dll/hook/hooks/SwapChainHook.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/render/Font.hpp"
#include "dll/render/GraphicsContext.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Velyx";

long long queryTicks() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

long long queryFrequency() {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

std::filesystem::path selfDirectory(HMODULE self) {
    wchar_t buffer[MAX_PATH]{};
    if (self && GetModuleFileNameW(self, buffer, MAX_PATH)) {
        return std::filesystem::path(buffer).parent_path();
    }
    return {};
}

}

Velyx& Velyx::get() {
    static Velyx instance;
    return instance;
}

double Velyx::uptime() const {
    if (ticksPerSecond_ == 0) return 0.0;
    return static_cast<double>(queryTicks() - startTicks_) / static_cast<double>(ticksPerSecond_);
}

void Velyx::start(HMODULE self) {
    self_ = self;
    ticksPerSecond_ = queryFrequency();
    startTicks_ = queryTicks();
    lastFrameTicks_ = startTicks_;

    running_.store(true, std::memory_order_release);
    bootstrap();
}

void Velyx::bootstrap() {

    Paths::ensureLayout();

#ifdef VELYX_DEBUG
    Log::setMinimumLevel(LogLevel::Debug);
    Log::init(Paths::logs() / "velyx.log", true);
#else
    Log::init(Paths::logs() / "velyx.log", false);
#endif

    Log::info(kLog, "Velyx {} starting", version::kFull);

    ClientConfig& settings = config();
    settings.load();
    settings.markSessionStarted();

    crash::install();

    const bool safeMode = settings.shouldStartInSafeMode();
    if (safeMode) {
        Log::warn(kLog, "starting in safe mode after {} crashes", settings.crashStreak);
    }

    sdk::bindGame();

    Signatures& signatures = Signatures::get();
    Log::info(kLog, "Minecraft {}", signatures.gameVersion());
    signatures.resolveAll();

    if (!signatures.healthy()) {
        Log::warn(kLog,
                  "some signatures are missing; modules that read the game stay in reduced "
                  "mode (see assets/signatures/)");
    }

    ThemeManager::get().load();

    bindServices();
    Playtime::get().load();
    updates::check(settings.updateChannel);

    ModuleManager& manager = modules();
    manager.setSafeMode(safeMode);
    manager.initialize();

    ProfileManager& profileManager = profiles();
    profileManager.load();
    profileManager.switchTo(settings.activeProfile);

    if (!settings.onboardingCompleted) {
        if (Module* onboarding = manager.find("onboarding")) {
            onboarding->setEnabled(true, false);
        }
    }

    HookManager& hooks = HookManager::get();
    hooks.add<SwapChainHook>();
    hooks.add<WindowHook>();

    SwapChainHook::setPresentCallback([this](IDXGISwapChain* swapChain) { onPresent(swapChain); });
    SwapChainHook::setResizeCallback(
        [this](unsigned width, unsigned height) { onResize(width, height); });

    events().on<DeviceLostEvent>(this, &Velyx::onDeviceLost, EventPriority::First);

    if (!hooks.installAll()) {
        Log::fatal(kLog, "hooks could not be installed, stopping");
        shutdown();
        return;
    }

    ready_.store(true, std::memory_order_release);
    Log::info(kLog, "ready: {} module(s), profile '{}', theme '{}'", manager.all().size(),
              profileManager.current().name, theme().name);
}

void Velyx::initialiseGraphicsOnce() {
    if (graphicsInitialised_) return;

    GraphicsContext& graphics = GraphicsContext::get();
    if (!graphics.ready()) return;

    FontManager& fonts = FontManager::get();
    fonts.initialize(graphics.dwrite());

    fonts.loadDirectory(selfDirectory(self_) / "assets" / "fonts");
    fonts.loadDirectory(Paths::assets() / "fonts");

    graphicsInitialised_ = true;
    Log::info(kLog, "renderer ready ({}x{})", static_cast<int>(graphics.size().x),
              static_cast<int>(graphics.size().y));
}

void Velyx::onPresent(IDXGISwapChain* swapChain) {
    if (!ready() || ejecting_.load(std::memory_order_acquire)) return;

    GraphicsContext& graphics = GraphicsContext::get();
    if (!graphics.attach(swapChain)) return;

    WindowHook::attach(graphics.window());

    initialiseGraphicsOnce();
    if (!graphicsInitialised_) return;

    const long long now = queryTicks();
    delta_ = ticksPerSecond_ > 0
                 ? static_cast<float>(static_cast<double>(now - lastFrameTicks_) /
                                      static_cast<double>(ticksPerSecond_))
                 : 0.f;
    lastFrameTicks_ = now;

    delta_ = clamp(delta_, 0.f, 0.25f);

    if (delta_ > 0.f) {
        const float instant = 1.f / delta_;
        fps_ = fps_ <= 0.f ? instant : approach(fps_, instant, delta_, 6.f);
    }

    ++frameIndex_;
    screenSize_ = graphics.size();

    FrameEvent frame;
    frame.deltaSeconds = delta_;
    frame.frameIndex = frameIndex_;
    frame.screenSize = screenSize_;
    events().emit(frame);

    if (!graphics.beginFrame()) return;

    if (renderer_.begin(graphics.d2d(), screenSize_, delta_)) {
        renderer_.setEffectsEnabled(theme().blur);

        RenderEvent render;
        render.renderer = &renderer_;
        render.screenSize = screenSize_;
        render.deltaSeconds = delta_;
        render.guiOpen = WindowHook::captureInput();
        events().emit(render);

        RenderTopEvent top;
        top.renderer = &renderer_;
        top.screenSize = screenSize_;
        top.deltaSeconds = delta_;
        events().emit(top);

        renderer_.end();
    }

    graphics.endFrame();
}

void Velyx::onResize(unsigned width, unsigned height) {
    SwapchainResizeEvent event;
    event.width = width;
    event.height = height;
    events().emit(event);

    FontManager::get().invalidate();
}

void Velyx::onDeviceLost(DeviceLostEvent& event) {
    renderer_.onDeviceLost();
    FontManager::get().invalidate();
    graphicsInitialised_ = false;
}

void Velyx::requestEject() {
    if (ejecting_.exchange(true)) return;
    Log::info(kLog, "eject requested");
    shutdown();
}

void Velyx::shutdown() {
    ready_.store(false, std::memory_order_release);

    profiles().saveCurrent();
    config().markSessionEnded();

    HookManager::get().uninstallAll();
    crash::uninstall();
    updates::shutdown();
    WindowHook::setCaptureInput(false);

    modules().shutdown();
    events().clear();

    renderer_.onDeviceLost();
    FontManager::get().shutdown();
    GraphicsContext::get().shutdown();

    Log::info(kLog, "shut down cleanly");
    Log::shutdown();

    running_.store(false, std::memory_order_release);
}

}
