#include "Turn.hpp"

#include <atomic>
#include <cmath>

#include "core/Log.hpp"
#include "dll/event/EventBus.hpp"
#include "dll/event/Events.hpp"

namespace velyx::turn {
namespace {

// What one count is worth in degrees, nominally, on the input doors. See the note in
// the header: nothing that only scales or smooths a turn is affected by the value,
// because it goes in and comes back out through the same constant.
constexpr double kDegreesPerCount = 0.15;

// Whole counts are all a mouse message can carry, so the fraction a scaled or smoothed
// turn leaves behind is kept and paid into the next one. Without it a multiplier below
// one rounds every small movement down to nothing and the mouse feels stuck rather
// than slow. Owned by the thread that shapes.
double g_residual[2]{};

// Written by whoever asks for a turn, drained by the thread that shapes. Millidegrees
// so that asking is a single atomic add rather than a lock: shaping must never hold
// one across the event below, which a handler on another thread reaches into.
std::atomic<int64_t> g_injected[2]{};

constexpr const char* kLog = "Turn";

std::atomic<bool> g_carried{false};
std::atomic<bool> g_exact{false};

float takeInjected(size_t axis) {
    return static_cast<float>(
        static_cast<double>(g_injected[axis].exchange(0, std::memory_order_relaxed)) / 1000.0);
}

}

void shape(int64_t& x, int64_t& y) {
    // The game door shapes this same movement a little further down the road, once
    // it has become degrees. Counts shaped here as well would be shaped twice.
    if (g_exact.load(std::memory_order_acquire)) return;

    const int64_t counted[2]{x, y};

    TurnDeltaEvent event;
    event.yaw = static_cast<float>(static_cast<double>(x) * kDegreesPerCount);
    event.pitch = static_cast<float>(static_cast<double>(y) * kDegreesPerCount);

    const float offered[2]{event.yaw, event.pitch};

    // Shaping runs inside a detour on the game's own thread: an exception leaving it
    // reaches no handler and ends the process through std::terminate. A throw costs
    // the movement its shaping, nothing more.
    try {
        events().emit(event);
    } catch (...) {
        return;
    }

    const float shaped[2]{event.yaw, event.pitch};

    double wanted[2]{0.0, 0.0};
    if (!event.cancelled) {
        for (size_t i = 0; i < 2; ++i) {
            // Degrees are a float, so an axis that goes out through the constant and
            // comes back unchanged still loses a count to rounding. An axis nobody
            // shaped is therefore handed back exactly as it arrived: a mouse with no
            // module on it must not feel different for having been offered.
            wanted[i] = shaped[i] == offered[i] ? static_cast<double>(counted[i])
                                                : static_cast<double>(shaped[i]) / kDegreesPerCount;
        }
    }

    int64_t* const out[]{&x, &y};
    for (size_t i = 0; i < 2; ++i) {
        const double injected = static_cast<double>(takeInjected(i)) / kDegreesPerCount;

        const double total = wanted[i] + injected + g_residual[i];
        const double whole = std::trunc(total);

        g_residual[i] = total - whole;
        *out[i] = static_cast<int64_t>(whole);
    }
}

void shapeDegrees(float& yaw, float& pitch) {
    TurnDeltaEvent event;
    event.yaw = yaw;
    event.pitch = pitch;
    event.exact = true;

    // Same boundary as above: this runs inside the game's own function.
    try {
        events().emit(event);
    } catch (...) {
        return;
    }

    float wanted[2]{0.f, 0.f};
    if (!event.cancelled) {
        wanted[0] = event.yaw;
        wanted[1] = event.pitch;
    }

    // Degrees in, degrees out: nothing is rounded, so nothing is owed to the next
    // movement. A handler that hands back something that is not a number has broken,
    // and the player's own movement is worth more than its opinion.
    float* const out[]{&yaw, &pitch};
    for (size_t i = 0; i < 2; ++i) {
        const float total = wanted[i] + takeInjected(i);
        if (std::isfinite(total)) *out[i] = total;
    }
}

void inject(float yawDegrees, float pitchDegrees) {
    const float degrees[]{yawDegrees, pitchDegrees};
    for (size_t i = 0; i < 2; ++i) {
        if (!std::isfinite(degrees[i]) || degrees[i] == 0.f) continue;
        g_injected[i].fetch_add(static_cast<int64_t>(std::llround(degrees[i] * 1000.f)),
                                std::memory_order_relaxed);
    }
}

void markCarried(const char* door) {
    if (g_carried.exchange(true, std::memory_order_acq_rel)) return;
    Log::info(kLog, "the game takes its mouse through {}", door);
}

bool carried() { return g_carried.load(std::memory_order_acquire); }

void markExact(bool exact) {
    if (g_exact.exchange(exact, std::memory_order_acq_rel) == exact) return;
    if (exact) {
        Log::info(kLog, "turns are shaped in the game's own degrees; the input doors let them pass");
    } else {
        Log::info(kLog, "the game door stood down; turns are shaped at the input again");
    }
}

bool exact() { return g_exact.load(std::memory_order_acquire); }

void forget() {
    g_residual[0] = g_residual[1] = 0.0;
    g_injected[0].store(0, std::memory_order_relaxed);
    g_injected[1].store(0, std::memory_order_relaxed);
}

}
