#include "WindowHook.hpp"

#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <format>
#include <string>

#include "core/Log.hpp"
#include "core/Strings.hpp"
#include "dll/event/Events.hpp"
#include "dll/hook/hooks/UserInputHook.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Input";

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
std::atomic<bool> g_capture{false};

// attach() runs from Present, so it stamps the render thread. Whether the window
// procedure lands on that same thread decides whether every module's input handler
// races the frame it is drawn in, so it is worth saying out loud, once.
std::atomic<uint32_t> g_renderThread{0};
std::atomic<bool> g_threadingReported{false};

std::mutex g_titleMutex;
std::wstring g_pendingTitle;
std::atomic<bool> g_titlePending{false};
std::atomic<bool> g_releasePending{false};

// 0 nothing, 1 show the pointer, 2 take it back. Handed to the window's own thread for
// the same reason the title is — and for a sharper one. The count that decides whether
// a cursor is drawn belongs to the input queue of the thread that owns the foreground
// window, which here is the thread the messages run on, not the one the frames do.
// Asking for it from the render thread moved a counter nobody looks at, and the
// interface came up with no pointer on it at all.
std::atomic<int> g_cursorPending{0};

Vec2 g_mouse;
bool g_keys[256]{};

// GetKeyState answers for the calling thread's queue, and the answer it gives inside
// a game's own procedure is not always the truth — a bind with Ctrl in it would then
// never match, however hard the key is held. Every key already passes through here, so
// what was seen is asked first and GetKeyState is only the second opinion.
//
// And it has to be the *real* GetKeyState. The releases below wipe this record when an
// interface opens, so a Ctrl held from before it opened is not in it; user32 is then
// the only one who still knows, and user32 is busy telling the game that nothing is
// held at all. Ctrl+K would have opened the menu and been unable to close it.
bool modifierHeld(int generic, int left, int right) {
    if (g_keys[generic] || g_keys[left] || g_keys[right]) return true;
    return (UserInputHook::realKeyState(generic) & 0x8000) != 0;
}

void fillModifiers(KeyEvent& event) {
    event.shift = modifierHeld(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
    event.ctrl = modifierHeld(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
    event.alt = modifierHeld(VK_MENU, VK_LMENU, VK_RMENU);
}

MouseButton buttonFor(UINT message, WPARAM wParam) {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            return MouseButton::Left;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
            return MouseButton::Right;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
            return MouseButton::Middle;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
            return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? MouseButton::X1 : MouseButton::X2;
        default:
            return MouseButton::None;
    }
}

int virtualKeyFor(MouseButton button) {
    switch (button) {
        case MouseButton::Left:   return VK_LBUTTON;
        case MouseButton::Right:  return VK_RBUTTON;
        case MouseButton::Middle: return VK_MBUTTON;
        case MouseButton::X1:     return VK_XBUTTON1;
        case MouseButton::X2:     return VK_XBUTTON2;
        case MouseButton::None:   break;
    }
    return 0;
}

}

WindowHook::WindowHook() : Hook("Window", 0) {}

bool WindowHook::install() {

    installed_ = true;
    return true;
}

void WindowHook::uninstall() {
    if (g_window && g_originalProc) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalProc));
        g_originalProc = nullptr;
        g_window = nullptr;
    }
    installed_ = false;
}

void WindowHook::attach(HWND window) {
    if (!window || window == g_window) return;

    if (g_window && g_originalProc) {
        SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalProc));
    }

    g_window = window;
    g_originalProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowHook::wndProc)));

    if (!g_originalProc) {
        Log::error(kLog, "SetWindowLongPtr failed, keyboard and mouse input are unavailable");
        return;
    }

    g_renderThread.store(GetCurrentThreadId(), std::memory_order_release);
    g_threadingReported.store(false, std::memory_order_release);

    Log::info(kLog, "attached to window {:#x}", reinterpret_cast<uintptr_t>(window));
}

HWND WindowHook::window() { return g_window; }

namespace {

// Straight to the game's own procedure: dispatch() swallows keys while the client
// is capturing, which is the very thing being worked around.
void releaseHeldKeys(HWND window) {
    if (!window || !g_originalProc) return;

    struct ButtonMessage {
        int key;
        UINT message;
        WPARAM wParam;
    };
    constexpr ButtonMessage kButtons[]{
        {VK_LBUTTON, WM_LBUTTONUP, 0},
        {VK_RBUTTON, WM_RBUTTONUP, 0},
        {VK_MBUTTON, WM_MBUTTONUP, 0},
        {VK_XBUTTON1, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON1)},
        {VK_XBUTTON2, WM_XBUTTONUP, MAKEWPARAM(0, XBUTTON2)},
    };

    for (int key = 0; key < 256; ++key) {
        if (!g_keys[key]) continue;
        g_keys[key] = false;

        const auto button = std::ranges::find_if(
            kButtons, [key](const ButtonMessage& entry) { return entry.key == key; });

        if (button != std::end(kButtons)) {
            CallWindowProcW(g_originalProc, window, button->message, button->wParam, 0);
            continue;
        }

        // Bit 31 says released, bit 30 that it was down, and the scan code is what a
        // real key-up carries.
        const LPARAM info = static_cast<LPARAM>(0xC0000001u) |
                            (static_cast<LPARAM>(MapVirtualKeyW(static_cast<UINT>(key),
                                                                MAPVK_VK_TO_VSC)) << 16);
        CallWindowProcW(g_originalProc, window, WM_KEYUP, static_cast<WPARAM>(key), info);
    }
}

}

void WindowHook::setCaptureInput(bool capture) {
    const bool previous = g_capture.exchange(capture);
    if (previous == capture) return;

    if (capture) {
        // Whatever was held when the client took over never gets its key-up: the
        // procedure swallows those from here on, and the game would be left sprinting
        // on a Ctrl that is no longer down. Opening the menu with Ctrl+K did exactly
        // that. The releases are handed to the window's own thread, like the title.
        //
        // Only the part of the story this procedure owns. Bedrock does not read its
        // keyboard from these messages at all — it reads raw input, which
        // UserInputHook empties, and GameInput readings, which GameInputHook empties.
        // This is for whatever does listen to the window.
        g_releasePending.store(true, std::memory_order_release);
    }

    g_cursorPending.store(capture ? 1 : 2, std::memory_order_release);

    if (g_window) PostMessageW(g_window, WM_NULL, 0, 0);
}

bool WindowHook::captureInput() { return g_capture.load(std::memory_order_relaxed); }

// SetWindowTextW sends WM_SETTEXT synchronously, so calling it from the render
// thread parks that thread until the window's own thread pumps — which is how the
// overlay froze the game. The title is queued and set from the procedure, where it
// is a plain call on the owning thread.
void WindowHook::setTitle(std::string_view title) {
    const std::wstring wide = strings::toUtf16(title);
    {
        const std::lock_guard<std::mutex> guard(g_titleMutex);
        g_pendingTitle = wide;
    }
    g_titlePending.store(true, std::memory_order_release);

    // An idle window sends no messages, so the queued title could sit there until the
    // player moves the mouse. A posted WM_NULL wakes the procedure on the very next
    // pump — and unlike SendMessage it returns immediately, so the render thread that
    // asked for the title never waits on the thread that owns the window.
    if (const HWND window = g_window) PostMessageW(window, WM_NULL, 0, 0);
}

Vec2 WindowHook::mousePosition() { return g_mouse; }

bool WindowHook::isKeyDown(int virtualKey) {
    if (virtualKey < 0 || virtualKey > 255) return false;
    return g_keys[virtualKey];
}

// For anything of the client's that needs to know about a modifier while an interface
// is open. Asking user32 there is no use: UserInputHook keeps its answers empty for as
// long as capture lasts, so the procedure's own record is the only truthful one.
bool WindowHook::isModifierDown(int genericKey) {
    switch (genericKey) {
        case VK_SHIFT:   return modifierHeld(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
        case VK_CONTROL: return modifierHeld(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
        case VK_MENU:    return modifierHeld(VK_MENU, VK_LMENU, VK_RMENU);
        default:         return isKeyDown(genericKey);
    }
}

std::string WindowHook::keyName(int virtualKey) {
    switch (virtualKey) {
        case 0:            return "None";
        case VK_LBUTTON:   return "Left click";
        case VK_RBUTTON:   return "Right click";
        case VK_MBUTTON:   return "Middle click";
        case VK_XBUTTON1:  return "Mouse 4";
        case VK_XBUTTON2:  return "Mouse 5";
        case VK_SPACE:     return "Space";
        case VK_ESCAPE:    return "Esc";
        case VK_RETURN:    return "Enter";
        case VK_TAB:       return "Tab";
        case VK_BACK:      return "Back";
        case VK_LSHIFT:    return "Left Shift";
        case VK_RSHIFT:    return "Right Shift";
        case VK_LCONTROL:  return "Left Ctrl";
        case VK_RCONTROL:  return "Right Ctrl";
        case VK_LMENU:     return "Left Alt";
        case VK_RMENU:     return "Right Alt";
        default:           break;
    }

    const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    if (scanCode == 0) return std::format("VK {}", virtualKey);

    wchar_t buffer[64]{};
    const int length = GetKeyNameTextW(static_cast<LONG>(scanCode << 16), buffer, 64);
    if (length <= 0) return std::format("VK {}", virtualKey);

    return strings::toUtf8(std::wstring_view(buffer, static_cast<size_t>(length)));
}

LRESULT CALLBACK WindowHook::wndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_threadingReported.exchange(true, std::memory_order_acq_rel)) {
        const uint32_t here = GetCurrentThreadId();
        const uint32_t render = g_renderThread.load(std::memory_order_acquire);
        if (here == render) {
            Log::info(kLog, "messages and frames share thread {}", here);
        } else {
            Log::warn(kLog, "messages run on thread {}, frames on thread {}", here, render);
        }
    }

    if (g_releasePending.exchange(false, std::memory_order_acq_rel)) releaseHeldKeys(window);

    // Both loops are bounded. They run on the thread that pumps the game's messages,
    // and a count that refuses to move would otherwise hang the game rather than the
    // cursor.
    if (const int cursor = g_cursorPending.exchange(0, std::memory_order_acq_rel)) {
        constexpr int kGuard = 64;
        if (cursor == 1) {
            ClipCursor(nullptr);
            for (int i = 0; i < kGuard && ShowCursor(TRUE) < 0; ++i) {}
        } else {
            for (int i = 0; i < kGuard && ShowCursor(FALSE) >= 0; ++i) {}
        }
    }

    if (g_titlePending.exchange(false, std::memory_order_acq_rel)) {
        std::wstring title;
        {
            const std::lock_guard<std::mutex> guard(g_titleMutex);
            title.swap(g_pendingTitle);
        }
        if (!title.empty()) SetWindowTextW(window, title.c_str());
    }

    const bool capturing = captureInput();

    try {
        return dispatch(window, message, wParam, lParam, capturing);
    } catch (const std::exception& error) {
        Log::error(kLog, "input handler threw: {}", error.what());
    } catch (...) {
        Log::error(kLog, "input handler threw");
    }

    return CallWindowProcW(g_originalProc, window, message, wParam, lParam);
}

namespace {

bool isInputMessage(UINT message) {
    switch (message) {
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_INPUT:
            return true;
        default:
            return false;
    }
}

}

// Swallowing a message in this procedure only helps against a game that reads its input
// from one. Bedrock does not: it takes what it wants out of the pump itself, before
// DispatchMessage has been called at all, which is why its own buttons went on being
// clicked through an open menu. So the message is taken from it there — the client is
// handed it directly, and what is left in the queue is a WM_NULL that means nothing to
// anyone.
bool WindowHook::interceptForClient(MSG* message) {
    if (!message || !captureInput()) return false;
    if (message->hwnd != g_window || !isInputMessage(message->message)) return false;

    // A key still has to become a character, or the search field goes deaf. This posts
    // the WM_CHAR, which comes back through this same door a turn of the pump later.
    if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) {
        TranslateMessage(message);
    }

    // Told it is capturing, the body below emits the events the interface lives on and
    // hands the game's own procedure nothing. Guarded like the procedure itself is:
    // this returns into the game's own pump, and a module that throws there would take
    // the game down with it.
    try {
        dispatch(message->hwnd, message->message, message->wParam, message->lParam, true);
    } catch (const std::exception& error) {
        Log::error(kLog, "input handler threw: {}", error.what());
    } catch (...) {
        Log::error(kLog, "input handler threw");
    }

    // Raw input is the one message the system still wants handed back when it is being
    // withheld: what sits behind it is only released when someone reads it or the
    // default procedure sees it, and the game is about to do neither.
    if (message->message == WM_INPUT) {
        DefWindowProcW(message->hwnd, message->message, message->wParam, message->lParam);
    }

    message->message = WM_NULL;
    message->wParam = 0;
    message->lParam = 0;
    return true;
}

LRESULT WindowHook::dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                             bool capturing) {
    switch (message) {
        case WM_MOUSEMOVE: {
            g_mouse = {static_cast<float>(GET_X_LPARAM(lParam)),
                       static_cast<float>(GET_Y_LPARAM(lParam))};

            MouseEvent event;
            event.action = MouseAction::Move;
            event.position = g_mouse;
            events().emit(event);

            if (capturing) return 0;
            break;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP: {
            const MouseButton button = buttonFor(message, wParam);
            const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
                              message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
                              message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                              message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;

            if (const int vk = virtualKeyFor(button); vk != 0) g_keys[vk] = down;

            g_mouse = {static_cast<float>(GET_X_LPARAM(lParam)),
                       static_cast<float>(GET_Y_LPARAM(lParam))};

            MouseEvent event;
            event.button = button;
            event.action = down ? MouseAction::Press : MouseAction::Release;
            event.position = g_mouse;
            events().emit(event);

            KeyEvent keyEvent;
            keyEvent.key = virtualKeyFor(button);
            keyEvent.down = down;
            fillModifiers(keyEvent);
            events().emit(keyEvent);

            if (capturing || event.cancelled) return 0;
            break;
        }

        case WM_MOUSEWHEEL: {
            MouseEvent event;
            event.action = MouseAction::Wheel;
            event.position = g_mouse;
            event.wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
            events().emit(event);

            if (capturing || event.cancelled) return 0;
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const int key = static_cast<int>(wParam);

            if (key >= 0 && key <= 255) g_keys[key] = down;

            KeyEvent event;
            event.key = key;
            event.down = down;
            event.repeat = down && (lParam & (1 << 30)) != 0;
            fillModifiers(event);
            events().emit(event);

            if (event.cancelled) return 0;

            if (capturing) return 0;
            break;
        }

        case WM_CHAR: {
            CharEvent event;
            event.codepoint = static_cast<unsigned int>(wParam);
            events().emit(event);

            if (capturing || event.cancelled) return 0;
            break;
        }

        case WM_INPUT:
            // Said once, and only from behind an open interface: raw input still
            // arriving here means the game is picking it out of the pump itself rather
            // than from this procedure, and that swallowing it here settles nothing.
            if (capturing) {
                static std::atomic<bool> said{false};
                if (!said.exchange(true, std::memory_order_acq_rel)) {
                    Log::info(kLog, "raw input arrives while an interface is open");
                }
                // Handed back for its cleanup, and to nobody else. Same reason as in
                // interceptForClient above.
                DefWindowProcW(window, message, wParam, lParam);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (capturing) {
                SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
                return TRUE;
            }
            break;

        case WM_KILLFOCUS:

            std::memset(g_keys, 0, sizeof(g_keys));
            break;

        case WM_CLOSE:
        case WM_DESTROY: {
            GameClosingEvent closing;
            events().emit(closing);
            break;
        }

        case WM_ENTERSIZEMOVE:
        case WM_EXITSIZEMOVE: {
            WindowDragEvent event;
            event.dragging = message == WM_ENTERSIZEMOVE;
            events().emit(event);
            break;
        }

        case WM_SIZE: {
            SwapchainResizeEvent event;
            event.width = static_cast<uint32_t>(LOWORD(lParam));
            event.height = static_cast<uint32_t>(HIWORD(lParam));
            event.fromWindow = true;
            Log::debug(kLog, "WM_SIZE {}x{}", event.width, event.height);
            events().emit(event);
            break;
        }

        default:
            break;
    }

    return CallWindowProcW(g_originalProc, window, message, wParam, lParam);
}

}
