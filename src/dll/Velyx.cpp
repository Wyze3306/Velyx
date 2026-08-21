#include "Velyx.hpp"

#include <format>
#include <string_view>

#include <velyx/Version.hpp>

#include "core/Lang.hpp"
#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
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
#include "dll/ui/Ui.hpp"

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

    // A build injected straight from the build tree carries its assets beside it; an
    // install has them where the launcher unpacked them.
    std::error_code ec;
    const auto beside = selfDirectory(self) / "assets";
    assets_ = std::filesystem::exists(beside, ec) ? beside : Paths::assets();

    ticksPerSecond_ = queryFrequency();
    startTicks_ = queryTicks();
    lastFrameTicks_ = startTicks_;

    running_.store(true, std::memory_order_release);
    bootstrap();
}

std::filesystem::path Velyx::asset(std::string_view relative) const {
    return assets_ / std::filesystem::path(relative);
}

void Velyx::bootstrap() {

    Paths::ensureLayout();

#ifdef VELYX_DEBUG
    Log::setMinimumLevel(LogLevel::Debug);
    Log::init(Paths::logs() / "velyx.log", true);
#else
    // A release build injected into a game that dies without a word is the one case
    // where the whole trace is worth having; the game inherits this from whatever
    // launched it.
    if (GetEnvironmentVariableW(L"VELYX_VERBOSE", nullptr, 0) > 0) {
        Log::setMinimumLevel(LogLevel::Debug);
    }
    Log::init(Paths::logs() / "velyx.log", false);
#endif

    Log::info(kLog, "Velyx {} starting", version::kFull);

    ClientConfig& settings = config();
    settings.load();
    settings.markSessionStarted();

    lang::load(settings.language, asset("lang"));

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
    ThemeManager::get().apply(settings.theme);

    bindServices();
    Playtime::get().load();
    updates::check(settings.updateChannel);

    ModuleManager& manager = modules();
    manager.setSafeMode(safeMode);
    manager.initialize();

    // Before the profile: the interfaces read their own settings from the
    // configuration, and a profile has no say in them.
    manager.load(settings.interfaceState, ModuleManager::Interfaces::Only);

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

    events().on<DeviceLostEvent>(this, &Velyx::onDeviceLost, EventPriority::First);
    events().on<GameClosingEvent>(this, &Velyx::onGameClosing, EventPriority::First);

    // WM_SIZE reaches us before the game gets around to recreating its swapchain,
    // which makes it the earliest moment to stop holding its buffers.
    events().on<SwapchainResizeEvent>(this, &Velyx::onWindowResized, EventPriority::First);
    events().on<WindowDragEvent>(this, &Velyx::onWindowDrag, EventPriority::First);

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

    fonts.loadDirectory(asset("fonts"));

    // The title bar is the one place an injected client can say it is there without
    // drawing a pixel, and it survives into screenshots and screen shares.
    WindowHook::setTitle(std::format("Minecraft {} · Velyx {}",
                                     Signatures::get().gameVersion(), version::kFull));

    graphicsInitialised_ = true;
    Log::info(kLog, "renderer ready ({}x{})", static_cast<int>(graphics.size().x),
              static_cast<int>(graphics.size().y));
}

// Velyx runs inside Minecraft's own process, so anything that escapes one of its
// callbacks reaches no handler and ends the game through std::terminate — an exit
// with no exception and no backtrace, which is exactly what was being seen. Every
// boundary the game calls into is guarded, and a frame that throws twice in a row
// stops drawing rather than taking the game down with it.
void Velyx::onPresent(IDXGISwapChain* swapChain) {
    try {
        presentFrame(swapChain);
        renderFailures_ = 0;
    } catch (const std::exception& error) {
        onRenderFailure(error.what());
    } catch (...) {
        onRenderFailure("unknown exception");
    }
}

void Velyx::onRenderFailure(std::string_view reason) {
    Log::error(kLog, "frame aborted after '{}' (module: {}), failure {}", reason,
               crash::currentBreadcrumb(), renderFailures_ + 1);

    GraphicsContext::get().endFrame();

    if (++renderFailures_ >= 2) {
        Log::fatal(kLog, "two frames in a row failed, the overlay stops drawing");
        ready_.store(false, std::memory_order_release);
    }
}

void Velyx::presentFrame(IDXGISwapChain* swapChain) {
    if (!ready() || ejecting_.load(std::memory_order_acquire)) return;

    GraphicsContext& graphics = GraphicsContext::get();
    if (!graphics.attach(swapChain)) return;

    WindowHook::attach(graphics.window());

    if (graphics.takeResized()) {
        const Vec2 size = graphics.size();
        onSwapchainRebuilt(static_cast<unsigned>(size.x), static_cast<unsigned>(size.y));
    }

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

    modules().applyPendingToggles();

    if (!graphics.beginFrame()) return;

    if (renderer_.begin(graphics.d2d(), screenSize_, delta_)) {
        renderer_.setEffectsEnabled(theme().blur);

        ui().beginOverlayFrame();

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

        ui().endOverlayFrame();
        renderer_.end();
    }

    graphics.endFrame();
}

// The render thread's own announcement, made once the overlay has been rebuilt for a
// size it can trust. It runs here and nowhere else: the font cache and every module's
// layout belong to the thread that draws them, and the resize arrives on another.
void Velyx::onSwapchainRebuilt(unsigned width, unsigned height) {
    try {
        FontManager::get().invalidate();

        SwapchainResizeEvent event;
        event.width = width;
        event.height = height;
        events().emit(event);
    } catch (const std::exception& error) {
        Log::error(kLog, "relayout for {}x{} failed: {} (module: {})", width, height, error.what(),
                   crash::currentBreadcrumb());
    } catch (...) {
        Log::error(kLog, "relayout for {}x{} failed (module: {})", width, height,
                   crash::currentBreadcrumb());
    }
}

void Velyx::onWindowResized(SwapchainResizeEvent& event) {
    // Only the window is news. The event above is this same type, and letting it come
    // back through here would have every rebuild ask for another release: the overlay
    // would tear itself down and build itself back up for as long as the game ran.
    if (!event.fromWindow) return;

    GraphicsContext::get().releaseForResize();
}

void Velyx::onWindowDrag(WindowDragEvent& event) {
    GraphicsContext::get().setWindowMoving(event.dragging);
}

void Velyx::onGameClosing(GameClosingEvent& event) {
    if (closing_.exchange(true, std::memory_order_acq_rel)) return;

    // Only the session flag: the render thread is still running, so touching the
    // profiles or the modules from here would race the frame being drawn.
    config().markSessionEnded();
    Log::info(kLog, "the game is closing, session marked clean");
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
    ProfileManager::saveInterfaceState();
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
