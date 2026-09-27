#include "UserInputHook.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>

#include <MinHook.h>

#include "core/Log.hpp"
#include "dll/hook/Turn.hpp"
#include "dll/hook/hooks/WindowHook.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "UserInput";

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using PeekMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using GetMessageFn = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT);
using ShowCursorFn = int(WINAPI*)(BOOL);
using SetCursorFn = HCURSOR(WINAPI*)(HCURSOR);

GetAsyncKeyStateFn g_originalAsyncKeyState = nullptr;
GetKeyStateFn g_originalKeyState = nullptr;
GetKeyboardStateFn g_originalKeyboardState = nullptr;
GetCursorPosFn g_originalCursorPos = nullptr;
SetCursorPosFn g_originalSetCursorPos = nullptr;
ClipCursorFn g_originalClipCursor = nullptr;
GetRawInputDataFn g_originalRawInputData = nullptr;
GetRawInputBufferFn g_originalRawInputBuffer = nullptr;
PeekMessageFn g_originalPeekMessageW = nullptr;
PeekMessageFn g_originalPeekMessageA = nullptr;
GetMessageFn g_originalGetMessageW = nullptr;
GetMessageFn g_originalGetMessageA = nullptr;
ShowCursorFn g_originalShowCursor = nullptr;
SetCursorFn g_originalSetCursor = nullptr;

// Said once each, and only from behind an open interface: outside of one every game
// calls these constantly and the fact means nothing. Behind one it names the road the
// game's input is taking, which is the whole reason these are hooked at all.
void noteOnce(std::atomic<bool>& said, const char* what) {
    if (said.exchange(true, std::memory_order_acq_rel)) return;
    Log::info(kLog, "the game calls {} behind the interface", what);
}

std::atomic<bool> g_saidAsyncKeyState{false};
std::atomic<bool> g_saidKeyState{false};
std::atomic<bool> g_saidKeyboardState{false};
std::atomic<bool> g_saidCursorPos{false};
std::atomic<bool> g_saidSetCursorPos{false};
std::atomic<bool> g_saidClipCursor{false};
std::atomic<bool> g_saidRawInput{false};
std::atomic<bool> g_saidPump{false};
std::atomic<bool> g_saidShowCursor{false};
std::atomic<bool> g_saidSetCursor{false};

// Asked all the same, and only then emptied: the low bit of what this returns says the
// key has been pressed since it was last asked, and reading it is what clears it. Kept
// back rather than consumed, every key touched behind the interface would report itself
// as freshly pressed the moment the game asked again.
SHORT WINAPI asyncKeyStateDetour(int key) {
    const SHORT state = g_originalAsyncKeyState(key);
    if (!WindowHook::captureInput()) return state;

    noteOnce(g_saidAsyncKeyState, "GetAsyncKeyState");
    return 0;
}

// The client asks this too, through WindowHook — but by way of realKeyState below,
// which goes past this detour, so emptying it here costs the interface nothing.
SHORT WINAPI keyStateDetour(int key) {
    if (!WindowHook::captureInput()) return g_originalKeyState(key);
    noteOnce(g_saidKeyState, "GetKeyState");
    return 0;
}

BOOL WINAPI keyboardStateDetour(PBYTE keys) {
    const BOOL ok = g_originalKeyboardState(keys);
    if (ok && keys && WindowHook::captureInput()) {
        noteOnce(g_saidKeyboardState, "GetKeyboardState");
        // Only the "held" bit goes: the low bit says a key is toggled on, and Caps Lock
        // has no business turning itself off because a menu is open.
        for (int key = 0; key < 256; ++key) keys[key] &= 0x01;
    }
    return ok;
}

// Where the pointer was when the interface opened, so a game that follows the cursor
// itself sees it standing still rather than wandering over its own buttons.
std::atomic<LONG> g_restingX{0};
std::atomic<LONG> g_restingY{0};

BOOL WINAPI cursorPosDetour(LPPOINT point) {
    const BOOL ok = g_originalCursorPos(point);
    if (!ok || !point) return ok;

    if (WindowHook::captureInput()) {
        noteOnce(g_saidCursorPos, "GetCursorPos");
        point->x = g_restingX.load(std::memory_order_relaxed);
        point->y = g_restingY.load(std::memory_order_relaxed);
    } else {
        g_restingX.store(point->x, std::memory_order_relaxed);
        g_restingY.store(point->y, std::memory_order_relaxed);
    }
    return ok;
}

// A game that hides the pointer and recentres it every frame would drag the client's
// cursor to the middle of the screen with it. Told it succeeded, it leaves it alone.
BOOL WINAPI setCursorPosDetour(int x, int y) {
    if (!WindowHook::captureInput()) return g_originalSetCursorPos(x, y);
    noteOnce(g_saidSetCursorPos, "SetCursorPos");
    return TRUE;
}

// Letting the clip go is always allowed — the client does exactly that when it takes
// capture — and only a game asking to pen the cursor back in is refused.
BOOL WINAPI clipCursorDetour(const RECT* rect) {
    if (!rect || !WindowHook::captureInput()) return g_originalClipCursor(rect);
    noteOnce(g_saidClipCursor, "ClipCursor");
    return TRUE;
}

void empty(RAWINPUT& input) {
    if (input.header.dwType == RIM_TYPEMOUSE) {
        RAWMOUSE& mouse = input.data.mouse;
        mouse.usButtonFlags = 0;
        mouse.usButtonData = 0;
        mouse.ulRawButtons = 0;
        mouse.lLastX = 0;
        mouse.lLastY = 0;
        return;
    }

    if (input.header.dwType == RIM_TYPEKEYBOARD) {
        // A release rather than nothing at all: the key the player lets go of behind
        // the interface still has to reach the game, or the game goes on holding it
        // once the menu closes. A press turned into a release is a no-op to a game
        // that already believes the key is up.
        RAWKEYBOARD& keyboard = input.data.keyboard;
        const bool system =
            keyboard.Message == WM_SYSKEYDOWN || keyboard.Message == WM_SYSKEYUP;

        keyboard.Flags |= RI_KEY_BREAK;
        keyboard.Message = system ? WM_SYSKEYUP : WM_KEYUP;
    }
}

// The last road out of the procedure, and the one the client cannot simply refuse:
// the game reads its input straight out of the pump, so WindowHook takes each message
// there instead — the interface is handed it, and a WM_NULL is left behind.
void notePump() {
    if (g_saidPump.exchange(true, std::memory_order_acq_rel)) return;
    Log::info(kLog, "the game takes its input from the pump, before the procedure sees it");
}

// Only when the message is being taken out of the queue. Peeked and left there, it
// comes round again, and the interface would be handed it twice.
BOOL WINAPI peekMessageWDetour(LPMSG message, HWND window, UINT first, UINT last, UINT remove) {
    const BOOL got = g_originalPeekMessageW(message, window, first, last, remove);
    if (got && (remove & PM_REMOVE) && WindowHook::interceptForClient(message)) notePump();
    return got;
}

BOOL WINAPI peekMessageADetour(LPMSG message, HWND window, UINT first, UINT last, UINT remove) {
    const BOOL got = g_originalPeekMessageA(message, window, first, last, remove);
    if (got && (remove & PM_REMOVE) && WindowHook::interceptForClient(message)) notePump();
    return got;
}

BOOL WINAPI getMessageWDetour(LPMSG message, HWND window, UINT first, UINT last) {
    const BOOL got = g_originalGetMessageW(message, window, first, last);
    if (got > 0 && WindowHook::interceptForClient(message)) notePump();
    return got;
}

BOOL WINAPI getMessageADetour(LPMSG message, HWND window, UINT first, UINT last) {
    const BOOL got = g_originalGetMessageA(message, window, first, last);
    if (got > 0 && WindowHook::interceptForClient(message)) notePump();
    return got;
}

// A game in play hides the pointer, and it goes on hiding it with a menu of ours on
// top: the client showed the cursor when it took capture and the very next frame took
// it away again, which left the interface to be used blind.
//
// The hides are counted rather than carried out. Counted matters: a game that loops
// until the count goes negative — which is the usual way of hiding a cursor for good —
// would never come out of that loop if it were simply told no. What it believes it
// owes is made real again the moment the interface closes.
std::atomic<int> g_owedHides{0};

int WINAPI showCursorDetour(BOOL show) {
    if (WindowHook::captureInput()) {
        // Asking for the pointer always goes through — the client asks for it itself
        // the moment it takes capture — and only the hiding is held back.
        if (show) return g_originalShowCursor(TRUE);

        noteOnce(g_saidShowCursor, "ShowCursor");
        return g_owedHides.fetch_sub(1) - 1;
    }

    // Far enough below zero and the pointer is gone; going the whole way down would
    // only spin here for a game that asked once a frame.
    constexpr int kEnough = 64;
    for (int owed = std::max(g_owedHides.exchange(0), -kEnough); owed < 0; ++owed) {
        g_originalShowCursor(FALSE);
    }
    return g_originalShowCursor(show);
}

// The other half of the same habit: a null cursor is a hidden one, and an interface is
// no place for that either.
HCURSOR WINAPI setCursorDetour(HCURSOR cursor) {
    if (cursor || !WindowHook::captureInput()) return g_originalSetCursor(cursor);

    noteOnce(g_saidSetCursor, "SetCursor");
    return g_originalSetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
}

// Under WineGDK this is the door the mouse actually comes through: GameInput answers
// but the game never asks it. `lLastX/Y` are the movement itself rather than a running
// total, so shaping is a straight rewrite — the only care needed is a device reporting
// absolute coordinates, which is a position and not a movement at all.
void shapeOrEmpty(RAWINPUT& input, bool capturing) {
    if (capturing) {
        // Nothing the player did behind the menu reaches the game, and nothing a
        // module asked for does either: a turn belongs to play, not to a menu.
        turn::forget();
        empty(input);
        return;
    }

    if (input.header.dwType != RIM_TYPEMOUSE) return;

    // With the game's own door standing this movement is shaped there, a little
    // further down the road and in degrees; here it only passes.
    if (turn::exact()) return;

    RAWMOUSE& mouse = input.data.mouse;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;

    turn::markCarried("raw input");

    int64_t x = mouse.lLastX;
    int64_t y = mouse.lLastY;
    turn::shape(x, y);

    mouse.lLastX = static_cast<LONG>(x);
    mouse.lLastY = static_cast<LONG>(y);
}

UINT WINAPI rawInputDataDetour(HRAWINPUT handle, UINT command, LPVOID data, PUINT size,
                               UINT headerSize) {
    const UINT copied = g_originalRawInputData(handle, command, data, size, headerSize);
    if (copied == static_cast<UINT>(-1) || !data || command != RID_INPUT) return copied;

    const bool capturing = WindowHook::captureInput();
    if (capturing) noteOnce(g_saidRawInput, "raw input");
    shapeOrEmpty(*static_cast<RAWINPUT*>(data), capturing);
    return copied;
}

// NEXTRAWINPUTBLOCK spelled out: the macro aligns to a QWORD that mingw's headers do
// not declare.
PRAWINPUT nextBlock(PRAWINPUT entry) {
    constexpr size_t kAlign = sizeof(uint64_t);
    const size_t step = (entry->header.dwSize + kAlign - 1) & ~(kAlign - 1);
    return reinterpret_cast<PRAWINPUT>(reinterpret_cast<BYTE*>(entry) + step);
}

UINT WINAPI rawInputBufferDetour(PRAWINPUT data, PUINT size, UINT headerSize) {
    const UINT count = g_originalRawInputBuffer(data, size, headerSize);
    if (count == static_cast<UINT>(-1) || count == 0 || !data) return count;

    const bool capturing = WindowHook::captureInput();
    if (capturing) noteOnce(g_saidRawInput, "raw input");

    PRAWINPUT entry = data;
    for (UINT i = 0; i < count; ++i) {
        shapeOrEmpty(*entry, capturing);
        entry = nextBlock(entry);
    }
    return count;
}

}

SHORT UserInputHook::realKeyState(int key) {
    // Unhooked, the export is already the truth.
    return g_originalKeyState ? g_originalKeyState(key) : GetKeyState(key);
}

SHORT UserInputHook::realAsyncKeyState(int key) {
    return g_originalAsyncKeyState ? g_originalAsyncKeyState(key) : GetAsyncKeyState(key);
}

UserInputHook::UserInputHook() : Hook("userinput", 0) {}

bool UserInputHook::install() {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        Log::warn(kLog, "user32 is not loaded, which cannot be");
        return false;
    }

    const struct {
        const char* name;
        void* detour;
        void** original;
    } wanted[]{
        {"GetAsyncKeyState", reinterpret_cast<void*>(&asyncKeyStateDetour),
         reinterpret_cast<void**>(&g_originalAsyncKeyState)},
        {"GetKeyState", reinterpret_cast<void*>(&keyStateDetour),
         reinterpret_cast<void**>(&g_originalKeyState)},
        {"GetKeyboardState", reinterpret_cast<void*>(&keyboardStateDetour),
         reinterpret_cast<void**>(&g_originalKeyboardState)},
        {"GetCursorPos", reinterpret_cast<void*>(&cursorPosDetour),
         reinterpret_cast<void**>(&g_originalCursorPos)},
        {"SetCursorPos", reinterpret_cast<void*>(&setCursorPosDetour),
         reinterpret_cast<void**>(&g_originalSetCursorPos)},
        {"ClipCursor", reinterpret_cast<void*>(&clipCursorDetour),
         reinterpret_cast<void**>(&g_originalClipCursor)},
        {"GetRawInputData", reinterpret_cast<void*>(&rawInputDataDetour),
         reinterpret_cast<void**>(&g_originalRawInputData)},
        {"GetRawInputBuffer", reinterpret_cast<void*>(&rawInputBufferDetour),
         reinterpret_cast<void**>(&g_originalRawInputBuffer)},
        {"PeekMessageW", reinterpret_cast<void*>(&peekMessageWDetour),
         reinterpret_cast<void**>(&g_originalPeekMessageW)},
        {"PeekMessageA", reinterpret_cast<void*>(&peekMessageADetour),
         reinterpret_cast<void**>(&g_originalPeekMessageA)},
        {"GetMessageW", reinterpret_cast<void*>(&getMessageWDetour),
         reinterpret_cast<void**>(&g_originalGetMessageW)},
        {"GetMessageA", reinterpret_cast<void*>(&getMessageADetour),
         reinterpret_cast<void**>(&g_originalGetMessageA)},
        {"ShowCursor", reinterpret_cast<void*>(&showCursorDetour),
         reinterpret_cast<void**>(&g_originalShowCursor)},
        {"SetCursor", reinterpret_cast<void*>(&setCursorDetour),
         reinterpret_cast<void**>(&g_originalSetCursor)},
    };
    static_assert(std::size(wanted) == sizeof(targets_) / sizeof(targets_[0]),
                  "every entry needs a slot to be removed from");

    int live = 0;
    for (size_t i = 0; i < std::size(wanted); ++i) {
        void* const address = reinterpret_cast<void*>(GetProcAddress(user32, wanted[i].name));
        if (!address) {
            Log::warn(kLog, "user32 has no {}", wanted[i].name);
            continue;
        }
        if (!createAt(address, wanted[i].detour, wanted[i].original)) continue;

        targets_[i] = address;
        ++live;
    }

    installed_ = live > 0;
    if (installed_) Log::info(kLog, "{}/{} user32 entries hooked", live, std::size(wanted));
    return installed_;
}

void UserInputHook::uninstall() {
    for (void*& target : targets_) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }

    installed_ = false;
}

}
