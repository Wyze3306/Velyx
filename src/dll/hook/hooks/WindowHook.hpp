#pragma once

#include <windows.h>

#include "core/Math.hpp"
#include "dll/hook/Hook.hpp"

namespace velyx {

class WindowHook final : public Hook {
public:
    WindowHook();

    bool install() override;
    void uninstall() override;

    static void attach(HWND window);

    static void setCaptureInput(bool capture);

    // Handed a message on its way out of the game's own pump. What the client needs is
    // taken from it and a WM_NULL is left in its place, so a game that reads its input
    // there rather than from this procedure gets nothing. True when it did so.
    static bool interceptForClient(MSG* message);

    // Handed over rather than applied on the spot: see the note by the implementation.
    static void setTitle(std::string_view title);
    [[nodiscard]] static bool captureInput();

    [[nodiscard]] static Vec2 mousePosition();
    [[nodiscard]] static bool isKeyDown(int virtualKey);

    // Shift, Ctrl or Alt, from what the procedure has seen rather than from user32:
    // see the note by the implementation.
    [[nodiscard]] static bool isModifierDown(int genericKey);

    [[nodiscard]] static std::string keyName(int virtualKey);

    [[nodiscard]] static HWND window();

private:
    static LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    // The body of the procedure, so wndProc itself is only the guard around it.
    static LRESULT dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                            bool capturing);
};

}
