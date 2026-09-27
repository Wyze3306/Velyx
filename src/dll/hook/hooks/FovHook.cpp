#include "FovHook.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

#include <MinHook.h>

#ifdef _MSC_VER
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define VELYX_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(_ReturnAddress())
#else
#define VELYX_RETURN_ADDRESS() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#endif

#include "core/Log.hpp"
#include "core/Math.hpp"
#include "dll/event/Events.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/hook/hooks/GameHooks.hpp"
#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"
#include "dll/sdk/Camera.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "FovHook";

// float LevelRendererPlayer::getFov(float, bool, ...). The float rides in the second
// register, the rest are forwarded whatever they are.
using GetFovFn = float(__fastcall*)(uintptr_t self, float a, void* c, void* d, void* e, void* f);

// void LevelRendererPlayer::setupCamera(mce::Camera&, const float). Same forwarding.
using SetupCameraFn = int64_t(__fastcall*)(uintptr_t self, uintptr_t camera, float partial,
                                           void* d, void* e, void* f);

// float tanf(float). The C runtime's, which every caller in the game shares.
using TangentFn = float(__cdecl*)(float);

GetFovFn g_originalGetFov = nullptr;
SetupCameraFn g_originalSetupCamera = nullptr;
TangentFn g_originalTangent = nullptr;

// Where the game's calls to tanf for the field of view come back to: setupCamera's own,
// and the one in the function it hands the camera to, which builds the projection the
// frame is drawn with. Only a call returning to one of them is the field of view; every
// other caller gets the runtime's answer untouched.
uintptr_t g_tangentReturns[2]{};

std::atomic<bool> g_live{false};
std::atomic<bool> g_threw{false};
std::atomic<bool> g_reported{false};
std::atomic<bool> g_shapeRefused{false};

// A field of view under this is not one in degrees.
constexpr float kRadiansBelow = 3.5f;

// Offers the field of view to the handlers and returns what they made of it, in
// degrees; zero when their answer is not one the game could draw with.
float offer(float degrees) {
    FovEvent event;
    event.fov = degrees;

    sdk::camera().setGameFov(degrees);
    events().emit(event);

    if (!std::isfinite(event.fov) || event.fov < 1.f || event.fov > 179.f) return 0.f;
    return event.fov;
}

float __fastcall getFovDetour(uintptr_t self, float a, void* c, void* d, void* e, void* f) {
    if (!g_originalGetFov) return a;

    const float raw = g_originalGetFov(self, a, c, d, e, f);
    if (!std::isfinite(raw) || raw <= 0.f) return raw;

    const crash::Breadcrumb breadcrumb("fov hook");

    const bool radians = raw < kRadiansBelow;
    float answer = raw;

    hooks::guarded(kLog, "fov event", g_threw, [&] {
        if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
            Log::info(kLog, "the game asks for its field of view ({:.2f} {})", raw,
                      radians ? "rad" : "deg");
        }

        const float wanted = offer(radians ? toDegrees(raw) : raw);
        if (wanted > 0.f) answer = radians ? toRadians(wanted) : wanted;
    });

    return answer;
}

// The matrix on top of the camera's projection stack, once the game has built it. The
// stack is a ring of pointers: the entry before head plus count, wrapped, is the top.
// Every step is read through the guards, and the result has to look like a
// perspective projection — the two scales positive, the row that carries the divide
// by depth, nothing where nothing belongs — before a float of it is touched.
float* projectionOf(uintptr_t camera) {
    const int stackOffset = sig::offset(hooks::kProjectionStack);
    const int storageOffset = sig::offset(hooks::kStackStorage);
    const int capacityOffset = sig::offset(hooks::kStackCapacity);
    const int headOffset = sig::offset(hooks::kStackHead);
    const int countOffset = sig::offset(hooks::kStackCount);
    if (stackOffset < 0 || storageOffset < 0 || capacityOffset < 0 || headOffset < 0 ||
        countOffset < 0 || camera == 0) {
        return nullptr;
    }

    const uintptr_t stack = camera + static_cast<uintptr_t>(stackOffset);
    const auto storage = memory::read<uintptr_t>(stack + static_cast<uintptr_t>(storageOffset));
    const auto capacity = memory::read<uint64_t>(stack + static_cast<uintptr_t>(capacityOffset));
    const auto head = memory::read<uint64_t>(stack + static_cast<uintptr_t>(headOffset));
    const auto count = memory::read<uint64_t>(stack + static_cast<uintptr_t>(countOffset));

    if (storage == 0 || count == 0 || capacity == 0 || capacity > 4096 ||
        (capacity & (capacity - 1)) != 0) {
        return nullptr;
    }

    const uint64_t index = (head + count - 1) & (capacity - 1);
    const auto matrix = memory::read<uintptr_t>(storage + index * sizeof(uintptr_t));
    if (!memory::readable(reinterpret_cast<const void*>(matrix), 16 * sizeof(float))) {
        return nullptr;
    }

    auto* m = reinterpret_cast<float*>(matrix);
    const bool perspective = m[0] > 0.f && m[5] > 0.f && m[11] == -1.f && m[15] == 0.f &&
                             m[1] == 0.f && m[2] == 0.f && m[3] == 0.f && m[4] == 0.f &&
                             m[6] == 0.f && m[7] == 0.f && std::isfinite(m[0]) &&
                             std::isfinite(m[5]);
    return perspective ? m : nullptr;
}

void reshapeProjection(uintptr_t camera) {
    float* m = projectionOf(camera);
    if (!m) {
        if (!g_shapeRefused.exchange(true, std::memory_order_acq_rel)) {
            Log::warn(kLog, "the camera's projection is not where the pack says, or not a "
                            "perspective; the field of view stays the game's");
        }
        return;
    }

    // One entry of a perspective matrix is the cotangent of half the vertical field
    // of view, and the other scale is the same over the aspect ratio.
    const float halfTangent = 1.f / m[5];
    const float degrees = toDegrees(2.f * std::atan(halfTangent));
    if (!std::isfinite(degrees) || degrees <= 0.f) return;

    if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
        Log::info(kLog, "the game set its camera up with a {:.1f} degree field of view", degrees);
    }

    const float wanted = offer(degrees);
    if (wanted <= 0.f || std::abs(wanted - degrees) < 0.001f) return;

    const float scale = halfTangent / std::tan(toRadians(wanted) * 0.5f);
    if (!std::isfinite(scale) || scale <= 0.f) return;

    m[0] *= scale;
    m[5] *= scale;
}

// The one caller that matters hands in half the field of view, in radians, and builds
// the projection from what comes back. Answering with the tangent of half the field of
// view the handlers asked for is the whole of the zoom.
float __cdecl tangentDetour(float halfAngle) {
    if (!g_originalTangent) return 0.f;
    const uintptr_t caller = VELYX_RETURN_ADDRESS();
    if ((caller != g_tangentReturns[0] && caller != g_tangentReturns[1]) ||
        !std::isfinite(halfAngle) || halfAngle <= 0.f || halfAngle >= 1.6f) {
        return g_originalTangent(halfAngle);
    }

    const crash::Breadcrumb breadcrumb("fov hook");

    float angle = halfAngle;
    hooks::guarded(kLog, "fov tangent", g_threw, [&] {
        const float degrees = toDegrees(halfAngle * 2.f);
        if (!g_reported.exchange(true, std::memory_order_acq_rel)) {
            Log::info(kLog, "the game builds its camera from a {:.1f} degree field of view",
                      degrees);
        }

        const float wanted = offer(degrees);
        if (wanted > 0.f) angle = toRadians(wanted) * 0.5f;
    });

    return g_originalTangent(angle);
}

int64_t __fastcall setupCameraDetour(uintptr_t self, uintptr_t camera, float partial, void* d,
                                     void* e, void* f) {
    if (!g_originalSetupCamera) return 0;

    const int64_t result = g_originalSetupCamera(self, camera, partial, d, e, f);

    const crash::Breadcrumb breadcrumb("fov hook");
    hooks::guarded(kLog, "projection", g_threw, [&] { reshapeProjection(camera); });
    return result;
}

}

FovHook::FovHook() : Hook("FOV", 0) {}

bool FovHook::live() { return g_live.load(std::memory_order_acquire); }

bool FovHook::install() {
    // The getter where the build has one; the camera setup where it computes the
    // field of view into a component instead.
    if (const uintptr_t getter = sig::address(hooks::kGetFov); getter != 0) {
        target_ = reinterpret_cast<void*>(getter);
        if (!createAt(target_, reinterpret_cast<void*>(&getFovDetour),
                      reinterpret_cast<void**>(&g_originalGetFov))) {
            target_ = nullptr;
            return false;
        }
        g_live.store(true, std::memory_order_release);
        Log::info(kLog, "standing on {}", hooks::kGetFov);
        return true;
    }

    // The calls the tangent is taken through: `call [rip+x]`, six bytes, the displacement
    // two in. What each returns to is the instruction after it, and what the slot holds
    // is the runtime's tanf, which is what gets hooked, once, for both.
    uintptr_t tangent = 0;
    int sites = 0;
    for (const char* name : {hooks::kFovTangent, hooks::kDrawFovTangent}) {
        const uintptr_t call = sig::address(name);
        if (call == 0) continue;

        const auto displacement = memory::read<int32_t>(call + 2);
        const uintptr_t slot = call + 6 + static_cast<uintptr_t>(static_cast<intptr_t>(displacement));
        const auto function = memory::read<uintptr_t>(slot);
        if (memory::read<uint16_t>(call) != 0x15FF || function == 0 ||
            (tangent != 0 && function != tangent)) {
            Log::warn(kLog, "{} does not point at a call through the tanf import; left out", name);
            continue;
        }

        tangent = function;
        g_tangentReturns[sites++] = call + 6;
    }

    if (tangent != 0 && memory::readable(reinterpret_cast<const void*>(tangent), 16)) {
        target_ = reinterpret_cast<void*>(tangent);
        if (createAt(target_, reinterpret_cast<void*>(&tangentDetour),
                     reinterpret_cast<void**>(&g_originalTangent))) {
            g_live.store(true, std::memory_order_release);
            Log::info(kLog, "standing on the tangent the camera takes of the field of view ({} "
                            "call site{})",
                      sites, sites == 1 ? "" : "s");
            return true;
        }
        target_ = nullptr;
    }
    g_tangentReturns[0] = g_tangentReturns[1] = 0;

    if (const uintptr_t setup = sig::address(hooks::kSetupCamera); setup != 0) {
        for (const char* offset : {hooks::kProjectionStack, hooks::kStackStorage,
                                   hooks::kStackCapacity, hooks::kStackHead, hooks::kStackCount}) {
            if (sig::offset(offset) < 0) {
                Log::info(kLog, "{} is in the pack but {} is not: the field of view stays the game's",
                          hooks::kSetupCamera, offset);
                return false;
            }
        }

        target_ = reinterpret_cast<void*>(setup);
        if (!createAt(target_, reinterpret_cast<void*>(&setupCameraDetour),
                      reinterpret_cast<void**>(&g_originalSetupCamera))) {
            target_ = nullptr;
            return false;
        }
        g_live.store(true, std::memory_order_release);
        Log::info(kLog, "standing on {}: the field of view is read off the projection",
                  hooks::kSetupCamera);
        return true;
    }

    Log::info(kLog, "none of {}, {} or {} in the pack: nothing can change the field of view",
              hooks::kGetFov, hooks::kFovTangent, hooks::kSetupCamera);
    return false;
}

void FovHook::uninstall() {
    if (target_) {
        MH_DisableHook(target_);
        MH_RemoveHook(target_);
        target_ = nullptr;
    }
    installed_ = false;
    g_tangentReturns[0] = g_tangentReturns[1] = 0;
    g_live.store(false, std::memory_order_release);
    sdk::camera().setGameFov(0.f);
}

}
