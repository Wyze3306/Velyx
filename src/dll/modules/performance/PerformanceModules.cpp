#include "PerformanceModules.hpp"

#include <windows.h>

#include <dxgi1_4.h>
#include <mmsystem.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "core/Log.hpp"
#include "core/Strings.hpp"
#include "dll/Velyx.hpp"
#include "dll/feature/Services.hpp"
#include "dll/hook/hooks/WindowHook.hpp"
#include "dll/module/ModuleManager.hpp"
#include "dll/modules/hud/TextHud.hpp"
#include "dll/render/GraphicsContext.hpp"
#include "dll/sdk/Game.hpp"
#include "dll/ui/Notifications.hpp"
#include "dll/ui/Theme.hpp"

namespace velyx {
namespace {

long long ticks() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

long long tickRate() {
    static const long long rate = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    }();
    return rate;
}

bool gameHasFocus() {
    const HWND window = WindowHook::window();
    return window == nullptr || GetForegroundWindow() == window;
}

int displayRefreshRate() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return static_cast<int>(mode.dmDisplayFrequency);
}

// A cap the game never had, paced against the clock rather than against the frame
// before it, so a single slow frame does not drag every frame after it late.
//
// Two caps matter more than the headline one: what the game costs while you are
// reading something else, and what it costs sitting on a pause screen. Both are
// minutes at a time, at a hundred and something frames a second, for nothing.
class FrameLimiter final : public Module {
public:
    FrameLimiter()
        : Module("frame_limiter", "Frame limiter", ModuleCategory::Utility,
                 "Caps the framerate, with its own cap for a window you are not looking at.") {
        mutablePermissions().system = true;

        settings.header("In game");
        settings.intSlider("limit", "Cap", 120, 20, 500, "", " FPS");
        settings.toggle("matchDisplay", "Match the display instead", false,
                        "Uses the refresh rate Windows reports.");

        settings.header("Elsewhere");
        settings.toggle("capUnfocused", "Cap when the window is not in front", true);
        settings.intSlider("unfocusedLimit", "Then cap at", 20, 1, 144, "", " FPS");
        settings.toggle("capInMenus", "Cap on a menu screen", false);
        settings.intSlider("menuLimit", "Then cap at", 60, 10, 240, "", " FPS");

        settings.header("Pacing");
        settings.toggle("precise", "Tight pacing", true,
                        "Spins the last fraction of a millisecond. Steadier, slightly warmer.");
        settings.toggle("showTarget", "Say what it is capping to", false);

        settings.find("limit")->visibleWhen = [this] {
            return !settings.value<bool>("matchDisplay", false);
        };
        settings.find("unfocusedLimit")->visibleWhen = [this] {
            return settings.value<bool>("capUnfocused", true);
        };
        settings.find("menuLimit")->visibleWhen = [this] {
            return settings.value<bool>("capInMenus", false);
        };

        // The wait happens before the overlay is drawn, and on the frame event rather
        // than on the render one: what gets presented is then drawn after the wait
        // instead of before it, and the cap survives a frame the overlay sat out.
        on(&FrameLimiter::onFrame, EventPriority::Last);
        on(&FrameLimiter::onRender, EventPriority::Last);
        addKeywords({"fps", "limit", "cap", "framerate", "battery", "performance"});
    }

    void onEnable() override {
        // Without this a Sleep of one millisecond is a Sleep of fifteen, and the cap
        // lands wherever the scheduler feels like rather than where it was asked.
        timeBeginPeriod(1);
        raisedResolution_ = true;
        deadline_ = 0;
    }

    void onDisable() override {
        if (!raisedResolution_) return;
        timeEndPeriod(1);
        raisedResolution_ = false;
    }

private:
    [[nodiscard]] int currentTarget() const {
        if (settings.value<bool>("capUnfocused", true) && !gameHasFocus()) {
            return settings.value<int>("unfocusedLimit", 20);
        }
        if (settings.value<bool>("capInMenus", false) && sdk::game().inMenu()) {
            return settings.value<int>("menuLimit", 60);
        }
        if (settings.value<bool>("matchDisplay", false)) {
            const int refresh = displayRefreshRate();
            if (refresh > 0) return refresh;
        }
        return settings.value<int>("limit", 120);
    }

    void onFrame(FrameEvent&) {
        const int target = currentTarget();
        applied_ = target;
        if (target <= 0) return;

        const long long rate = tickRate();
        const long long period = rate / target;
        const long long now = ticks();

        if (deadline_ == 0 || now - deadline_ > rate) {
            // First frame, or the game was away long enough that catching up would mean
            // a burst of uncapped frames. Start again from here.
            deadline_ = now + period;
            return;
        }

        const bool precise = settings.value<bool>("precise", true);
        const long long spinMargin = precise ? rate / 2000 : 0;

        while (true) {
            const long long remaining = deadline_ - ticks();
            if (remaining <= 0) break;

            const long long milliseconds = (remaining - spinMargin) * 1000 / rate;
            if (milliseconds > 0) {
                Sleep(static_cast<DWORD>(milliseconds));
            } else if (precise) {
                YieldProcessor();
            } else {
                break;
            }
        }

        // Paced against the clock, so ordinary jitter is absorbed rather than added up.
        // A real stall is not jitter: past a couple of periods behind, the deadline is
        // moved rather than chased, or the frames after it arrive in a burst.
        deadline_ += period;
        if (ticks() - deadline_ > period * 2) deadline_ = ticks() + period;
    }

    void onRender(RenderTopEvent& event) {
        if (!settings.value<bool>("showTarget", false)) return;

        const int target = applied_;
        if (target <= 0) return;

        const Theme& active = theme();
        FontSpec spec;
        spec.family = active.monoFamily;
        spec.size = 11.f * active.fontScale;
        spec.weight = FontWeight::Medium;
        spec.align = TextAlign::Right;

        event.renderer->textShadowed(
            std::format("cap {} FPS", target),
            Rect{event.screenSize.x - 220.f, 6.f, event.screenSize.x - 12.f, 24.f},
            active.textMuted, spec);
    }

    long long deadline_ = 0;
    int applied_ = 0;
    bool raisedResolution_ = false;
};

// Windows schedules the game like any other window: normal priority, any core, and
// on a hybrid chip that includes the small ones. None of that is wrong; it is just
// not what you want while the thing in front of you is a fight.
class ProcessTuner final : public Module {
public:
    ProcessTuner()
        : Module("process_tuner", "Process tuning", ModuleCategory::Utility,
                 "Priority, core choice and timer resolution for the game's own process.") {
        mutablePermissions().system = true;

        settings.header("Scheduling");
        settings.dropdown("priority", "Priority", "Above normal",
                          {"Leave alone", "Above normal", "High"},
                          "High is for a machine doing nothing else.");
        settings.dropdown("affinity", "Cores", "Leave alone",
                          {"Leave alone", "Performance cores", "All but the first"},
                          "Performance cores needs a chip that has two kinds.");
        settings.toggle("timerResolution", "One millisecond timer", true,
                        "Sharpens every wait in the process, the game's own included.");

        settings.header("Memory");
        settings.toggle("trimOnWorldChange", "Release idle memory when a world closes", true);
        settings.button("trimNow", "Release idle memory now", [] { trim(); },
                        "Hands back what the process is holding but not using.");

        settings.header("Reporting");
        settings.toggle("notify", "Say what was applied", true);

        for (const char* id : {"priority", "affinity", "timerResolution"}) {
            settings.find(id)->onChange = [this] { apply(); };
        }

        on(&ProcessTuner::onWorldLeave);
        addKeywords({"priority", "affinity", "cores", "scheduler", "performance", "cpu"});
    }

    void onEnable() override { apply(); }

    void onDisable() override { restore(); }

private:
    static void trim() {
        // -1 on both is the documented "work out what you can spare" value.
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                 static_cast<SIZE_T>(-1));
    }

    void onWorldLeave(WorldLeaveEvent&) {
        if (settings.value<bool>("trimOnWorldChange", true)) trim();
    }

    // The cores Windows calls efficiency class zero on a hybrid chip are the small
    // ones. On a chip with only one kind every core is class zero, and the mask comes
    // back empty — which is the answer, not a failure: nothing is changed.
    static DWORD_PTR performanceCoreMask() {
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (length == 0) return 0;

        std::vector<uint8_t> buffer(length);
        auto* information =
            reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, information, &length)) {
            return 0;
        }

        BYTE best = 0;
        DWORD_PTR mask = 0;
        bool mixed = false;

        for (DWORD offset = 0; offset < length;) {
            const auto* entry =
                reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() +
                                                                                 offset);
            if (entry->Relationship == RelationProcessorCore) {
                const BYTE efficiency = entry->Processor.EfficiencyClass;
                if (efficiency != best && mask != 0) mixed = true;

                if (efficiency > best) {
                    best = efficiency;
                    mask = 0;
                }
                if (efficiency == best) {
                    for (WORD group = 0; group < entry->Processor.GroupCount; ++group) {
                        mask |= entry->Processor.GroupMask[group].Mask;
                    }
                }
            }
            offset += entry->Size;
        }

        return mixed ? mask : 0;
    }

    void apply() {
        if (!enabled()) return;

        const HANDLE process = GetCurrentProcess();
        std::vector<std::string> applied;

        if (savedPriority_ == 0) savedPriority_ = GetPriorityClass(process);
        if (savedAffinity_ == 0) {
            DWORD_PTR systemMask = 0;
            GetProcessAffinityMask(process, &savedAffinity_, &systemMask);
        }

        const std::string priority = settings.value<std::string>("priority", "Above normal");
        if (priority == "High") {
            SetPriorityClass(process, HIGH_PRIORITY_CLASS);
            applied.emplace_back("high priority");
        } else if (priority == "Above normal") {
            SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS);
            applied.emplace_back("raised priority");
        } else if (savedPriority_ != 0) {
            SetPriorityClass(process, savedPriority_);
        }

        const std::string affinity = settings.value<std::string>("affinity", "Leave alone");
        if (affinity == "Performance cores") {
            if (const DWORD_PTR mask = performanceCoreMask(); mask != 0) {
                SetProcessAffinityMask(process, mask);
                applied.emplace_back("performance cores");
            } else if (settings.value<bool>("notify", true)) {
                Notifications::info("Process tuning",
                                    "This processor has only one kind of core; the choice was "
                                    "left alone.");
            }
        } else if (affinity == "All but the first" && savedAffinity_ != 0) {
            const DWORD_PTR mask = savedAffinity_ & ~static_cast<DWORD_PTR>(1);
            if (mask != 0) {
                SetProcessAffinityMask(process, mask);
                applied.emplace_back("core 0 left free");
            }
        } else if (savedAffinity_ != 0) {
            SetProcessAffinityMask(process, savedAffinity_);
        }

        const bool wantsTimer = settings.value<bool>("timerResolution", true);
        if (wantsTimer && !raisedResolution_) {
            timeBeginPeriod(1);
            raisedResolution_ = true;
            applied.emplace_back("1 ms timer");
        } else if (!wantsTimer && raisedResolution_) {
            timeEndPeriod(1);
            raisedResolution_ = false;
        }

        if (applied.empty() || !settings.value<bool>("notify", true)) return;

        Notifications::success("Process tuning", strings::join(applied, ", "));
    }

    void restore() {
        const HANDLE process = GetCurrentProcess();

        if (savedPriority_ != 0) SetPriorityClass(process, savedPriority_);
        if (savedAffinity_ != 0) SetProcessAffinityMask(process, savedAffinity_);

        if (raisedResolution_) {
            timeEndPeriod(1);
            raisedResolution_ = false;
        }
    }

    DWORD savedPriority_ = 0;
    DWORD_PTR savedAffinity_ = 0;
    bool raisedResolution_ = false;
};

// What the process actually costs, from the two places that know: Windows for the
// processor and the memory, the display driver for the video memory. None of it comes
// from the game, so it reads the same in reduced mode as it does with a full pack.
class SystemMonitorHud final : public TextHud {
public:
    SystemMonitorHud()
        : TextHud("system_monitor", "System monitor",
                  "Processor, memory and video memory, as this process uses them.",
                  {0.99f, 0.3f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showCpu", "Processor", true);
        settings.toggle("showRam", "Memory", true);
        settings.toggle("showVram", "Video memory", true);
        settings.toggle("showThreads", "Threads", false);
        settings.toggle("showGpu", "Graphics card", false);

        settings.header("Thresholds");
        settings.toggle("colorThresholds", "Colour when it gets tight", true);
        settings.intSlider("vramWarn", "Video memory warning", 85, 50, 100, "", " %");
        settings.slider("interval", "Refresh every", 0.5f, 0.1f, 5.f, "", " s");

        addKeywords({"cpu", "ram", "vram", "memory", "system", "monitor", "performance"});
    }

    std::vector<Row> rows() override {
        refresh();

        const Theme& active = theme();
        const bool colour = settings.value<bool>("colorThresholds", true);

        std::vector<Row> result;

        if (settings.value<bool>("showCpu", true)) {
            Color tint{};
            if (colour && cpu_ > 85.f) tint = active.warning;
            result.push_back(Row{"CPU", std::format("{:.0f} %", cpu_), tint});
        }

        if (settings.value<bool>("showRam", true)) {
            result.push_back(Row{"RAM", std::format("{:.1f} GB", ramGb_), {}});
        }

        if (settings.value<bool>("showVram", true) && vramBudgetMb_ > 0) {
            const float share = static_cast<float>(vramUsedMb_) /
                                static_cast<float>(vramBudgetMb_) * 100.f;

            Color tint{};
            if (colour) {
                const auto warn = static_cast<float>(settings.value<int>("vramWarn", 85));
                if (share >= warn) tint = active.danger;
                else if (share >= warn - 15.f) tint = active.warning;
            }

            result.push_back(Row{"VRAM",
                                 std::format("{} / {} MB", vramUsedMb_, vramBudgetMb_), tint});
        }

        if (settings.value<bool>("showThreads", false)) {
            result.push_back(Row{"Threads", std::to_string(threads_), {}});
        }

        if (settings.value<bool>("showGpu", false) && !gpu_.empty()) {
            result.push_back(Row{"GPU", gpu_, {}});
        }

        return result;
    }

private:
    void refresh() {
        const long long now = ticks();
        const long long rate = tickRate();
        const auto interval = static_cast<long long>(
            settings.value<float>("interval", 0.5f) * static_cast<float>(rate));

        if (lastSample_ != 0 && now - lastSample_ < interval) return;

        const HANDLE process = GetCurrentProcess();

        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};

        if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
            const auto toTicks = [](const FILETIME& value) {
                return (static_cast<unsigned long long>(value.dwHighDateTime) << 32) |
                       value.dwLowDateTime;
            };
            const unsigned long long busy = toTicks(kernel) + toTicks(user);

            if (lastSample_ != 0 && busy >= lastBusy_) {
                SYSTEM_INFO info{};
                GetSystemInfo(&info);

                // Both sides in the same unit: process time is in hundreds of
                // nanoseconds, wall time in performance counter ticks.
                const double elapsedSeconds =
                    static_cast<double>(now - lastSample_) / static_cast<double>(rate);
                const double busySeconds = static_cast<double>(busy - lastBusy_) / 1.0e7;
                const auto cores = static_cast<double>(std::max<DWORD>(1, info.dwNumberOfProcessors));

                cpu_ = static_cast<float>(
                    clamp(busySeconds / (elapsedSeconds * cores) * 100.0, 0.0, 100.0));
            }
            lastBusy_ = busy;
        }

        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
            ramGb_ = static_cast<float>(counters.WorkingSetSize) / (1024.f * 1024.f * 1024.f);
        }

        if (IDXGIAdapter3* adapter = GraphicsContext::get().adapter()) {
            DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL,
                                                        &memory))) {
                vramUsedMb_ = static_cast<int>(memory.CurrentUsage / (1024ull * 1024ull));
                vramBudgetMb_ = static_cast<int>(memory.Budget / (1024ull * 1024ull));
            }

            if (gpu_.empty()) {
                DXGI_ADAPTER_DESC description{};
                if (SUCCEEDED(adapter->GetDesc(&description))) {
                    gpu_ = strings::toUtf8(description.Description);
                }
            }
        }

        // Only when it is being shown: the snapshot walks every thread on the machine,
        // which is a strange price for a line nobody asked for.
        threads_ = settings.value<bool>("showThreads", false) ? countThreads() : 0;
        lastSample_ = now;
    }

    static int countThreads() {
        // No cheap call gives this for the current process, which is why the line is
        // off by default and only counted while it is on.
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;

        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);

        const DWORD self = GetCurrentProcessId();
        int count = 0;

        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == self) ++count;
            } while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return count;
    }

    long long lastSample_ = 0;
    unsigned long long lastBusy_ = 0;
    float cpu_ = 0.f;
    float ramGb_ = 0.f;
    int vramUsedMb_ = 0;
    int vramBudgetMb_ = 0;
    int threads_ = 0;
    std::string gpu_;
};

// What Velyx itself costs, in microseconds, measured between the first thing it draws
// and the last. It exists so that "the client is what dropped my framerate" is a
// question with an answer rather than a suspicion.
class OverlayCost final : public TextHud {
public:
    OverlayCost()
        : TextHud("overlay_cost", "Overlay cost",
                  "How long the client's own drawing takes, and what it draws.",
                  {0.99f, 0.42f}, HudAnchor::TopRight) {
        addTextSettings(true);

        settings.header("Lines shown");
        settings.toggle("showTime", "Time per frame", true);
        settings.toggle("showShare", "Share of the frame", true);
        settings.toggle("showCalls", "Draw calls", true);
        settings.toggle("showText", "Text draws", false);
        settings.toggle("showBlurs", "Blurs", false);
        settings.slider("smoothing", "Smoothing", 6.f, 1.f, 30.f,
                        "Lower is steadier and slower to react.");

        // The two ends of the whole overlay pass: ahead of everything drawn on the
        // frame, and behind the last thing drawn on top of it.
        on(&OverlayCost::onPassStart, EventPriority::First);
        on(&OverlayCost::onPassEnd, EventPriority::Last);

        addKeywords({"overlay", "cost", "profiler", "client", "performance", "draw calls"});
    }

    std::vector<Row> rows() override {
        const Theme& active = theme();
        std::vector<Row> result;

        if (settings.value<bool>("showTime", true)) {
            Color tint{};
            if (microseconds_ > 2000.f) tint = active.warning;
            if (microseconds_ > 5000.f) tint = active.danger;
            result.push_back(Row{"Overlay", std::format("{:.0f} µs", microseconds_), tint});
        }

        if (settings.value<bool>("showShare", true)) {
            const float frameMicroseconds = Velyx::get().delta() * 1.0e6f;
            const float share =
                frameMicroseconds > 1.f ? microseconds_ / frameMicroseconds * 100.f : 0.f;
            result.push_back(Row{"Share", std::format("{:.1f} %", share), {}});
        }

        const Renderer::FrameStats& stats = lastStats_;

        if (settings.value<bool>("showCalls", true)) {
            result.push_back(Row{"Draws", std::to_string(stats.drawCalls), {}});
        }
        if (settings.value<bool>("showText", false)) {
            result.push_back(Row{"Text", std::to_string(stats.textDraws), {}});
        }
        if (settings.value<bool>("showBlurs", false)) {
            result.push_back(Row{"Blurs", std::to_string(stats.blurs), {}});
        }

        return result;
    }

private:
    void onPassStart(RenderEvent&) { started_ = ticks(); }

    void onPassEnd(RenderTopEvent& event) {
        if (started_ == 0) return;

        const auto sample = static_cast<float>(
            static_cast<double>(ticks() - started_) / static_cast<double>(tickRate()) * 1.0e6);

        microseconds_ = approach(microseconds_, sample, event.deltaSeconds,
                                 settings.value<float>("smoothing", 6.f));

        // Read here rather than in rows(): by the time the element is asked what to
        // print, the frame it is describing is the one still being drawn.
        lastStats_ = event.renderer->stats();
        started_ = 0;
    }

    long long started_ = 0;
    float microseconds_ = 0.f;
    Renderer::FrameStats lastStats_;
};

} // namespace

void registerPerformanceModules(ModuleManager& manager) {
    manager.add<FrameLimiter>();
    manager.add<ProcessTuner>();
    manager.add<SystemMonitorHud>();
    manager.add<OverlayCost>();
}

} // namespace velyx
