#include "Clipboard.hpp"

#include <windows.h>

#include <cstring>

namespace velyx::clipboard {

bool copy(std::string_view text) {
    if (!OpenClipboard(nullptr)) return false;

    bool copied = false;
    EmptyClipboard();

    if (const HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
        if (void* memory = GlobalLock(handle)) {
            std::memcpy(memory, text.data(), text.size());
            static_cast<char*>(memory)[text.size()] = '\0';
            GlobalUnlock(handle);
            copied = SetClipboardData(CF_TEXT, handle) != nullptr;
        }
        if (!copied) GlobalFree(handle);
    }

    CloseClipboard();
    return copied;
}

std::string read() {
    if (!OpenClipboard(nullptr)) return {};

    std::string text;
    if (const HANDLE handle = GetClipboardData(CF_TEXT)) {
        if (const char* memory = static_cast<const char*>(GlobalLock(handle))) {
            text.assign(memory, strnlen(memory, GlobalSize(handle)));
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return text;
}

}
