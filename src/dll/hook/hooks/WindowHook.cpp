#include "WindowHook.hpp"

#include <windowsx.h>

#include <atomic>
#include <cstring>
#include <format>
#include <string>

#include "core/Log.hpp"
#include "core/Strings.hpp"
#include "dll/event/Events.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Input";

HWND g_window = nullptr;
WNDPROC g_originalProc = nullptr;
std::atomic<bool> g_capture{false};
Vec2 g_mouse;
bool g_keys[256]{};

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
        Log::error(kLog, "SetWindowLongPtr a échoué : clavier et souris indisponibles");
        return;
    }

    Log::info(kLog, "attaché à la fenêtre {:#x}", reinterpret_cast<uintptr_t>(window));
}

HWND WindowHook::window() { return g_window; }

void WindowHook::setCaptureInput(bool capture) {
    const bool previous = g_capture.exchange(capture);
    if (previous == capture) return;

    if (capture) {

        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    } else {
        while (ShowCursor(FALSE) >= 0) {}
    }
}

bool WindowHook::captureInput() { return g_capture.load(std::memory_order_relaxed); }

Vec2 WindowHook::mousePosition() { return g_mouse; }

bool WindowHook::isKeyDown(int virtualKey) {
    if (virtualKey < 0 || virtualKey > 255) return false;
    return g_keys[virtualKey];
}

std::string WindowHook::keyName(int virtualKey) {
    switch (virtualKey) {
        case 0:            return "Aucune";
        case VK_LBUTTON:   return "Clic gauche";
        case VK_RBUTTON:   return "Clic droit";
        case VK_MBUTTON:   return "Clic molette";
        case VK_XBUTTON1:  return "Souris 4";
        case VK_XBUTTON2:  return "Souris 5";
        case VK_SPACE:     return "Espace";
        case VK_ESCAPE:    return "Échap";
        case VK_RETURN:    return "Entrée";
        case VK_TAB:       return "Tab";
        case VK_BACK:      return "Retour";
        case VK_LSHIFT:    return "Maj gauche";
        case VK_RSHIFT:    return "Maj droite";
        case VK_LCONTROL:  return "Ctrl gauche";
        case VK_RCONTROL:  return "Ctrl droit";
        case VK_LMENU:     return "Alt gauche";
        case VK_RMENU:     return "Alt droit";
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
    const bool capturing = captureInput();

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
            event.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            event.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            event.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
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

            if (capturing) return 0;
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

        case WM_SIZE: {
            SwapchainResizeEvent event;
            event.width = static_cast<uint32_t>(LOWORD(lParam));
            event.height = static_cast<uint32_t>(HIWORD(lParam));
            events().emit(event);
            break;
        }

        default:
            break;
    }

    return CallWindowProcW(g_originalProc, window, message, wParam, lParam);
}

}
