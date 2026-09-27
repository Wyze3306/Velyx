#include "ChatHook.hpp"

#include <atomic>
#include <string>

#include "core/Log.hpp"
#include "dll/event/Events.hpp"
#include "dll/feature/CrashReporter.hpp"
#include "dll/hook/hooks/GameHooks.hpp"
#include "dll/memory/Memory.hpp"
#include "dll/memory/Signatures.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Chat";

// void GuiData::displayChatMessage(std::string const& sender, std::string const& message)
using DisplayChatFn = void(__fastcall*)(uintptr_t self, const void* sender, const void* message);

// void GuiData::addMessage(GuiMessage message, int kind). The message travels by value,
// which on x64 means a pointer to the caller's copy, and the callee destroys it.
using AddMessageFn = void(__fastcall*)(uintptr_t self, uintptr_t message, uintptr_t kind,
                                       uintptr_t d);
using DestroyFn = void(__fastcall*)(uintptr_t message);

DisplayChatFn g_originalDisplay = nullptr;
AddMessageFn g_originalAdd = nullptr;
DestroyFn g_destroy = nullptr;

int g_authorOffset = -1;
int g_textOffset = -1;

std::atomic<bool> g_heard{false};
std::atomic<bool> g_keptLine{false};
std::atomic<bool> g_keptText{false};

// A listener that answers in chat comes back through here on the same thread. Once
// is a reply, twice is a loop.
thread_local bool t_inside = false;

struct Verdict {
    bool cancelled = false;
    bool rewritten = false;
    std::string sender;
    std::string message;
};

// Runs the listeners on the game's own thread. Nothing that escapes a detour reaches
// a handler, it ends the process through std::terminate; so everything a listener
// might throw stops here, and the line goes on as it came.
Verdict hear(const std::string& sender, const std::string& message) {
    Verdict verdict;
    if (!g_heard.exchange(true, std::memory_order_acq_rel)) {
        Log::info(kLog, "the game shows its first chat line");
    }

    t_inside = true;
    try {
        ChatReceiveEvent event;
        event.sender = sender;
        event.message = message;
        event.rawMessage = message;
        events().emit(event);

        verdict.cancelled = event.cancelled;
        verdict.rewritten = event.sender != sender || event.message != message;
        verdict.sender = std::move(event.sender);
        verdict.message = std::move(event.message);
    } catch (const std::exception& error) {
        Log::error(kLog, "a chat listener threw: {}", error.what());
        verdict = {};
    } catch (...) {
        Log::error(kLog, "a chat listener threw");
        verdict = {};
    }
    t_inside = false;
    return verdict;
}

void __fastcall displayDetour(uintptr_t self, const void* sender, const void* message) {
    if (!g_originalDisplay) return;
    if (!message || t_inside) {
        g_originalDisplay(self, sender, message);
        return;
    }

    const crash::Breadcrumb breadcrumb("chat hook");

    const std::string originalSender = memory::readString(reinterpret_cast<uintptr_t>(sender));
    const std::string originalMessage = memory::readString(reinterpret_cast<uintptr_t>(message));
    const Verdict verdict = hear(originalSender, originalMessage);

    if (verdict.cancelled) return;
    if (!verdict.rewritten) {
        g_originalDisplay(self, sender, message);
        return;
    }

    const memory::GameString outSender(verdict.sender);
    const memory::GameString outMessage(verdict.message);
    g_originalDisplay(self, outSender.as(), outMessage.as());
}

void __fastcall addDetour(uintptr_t self, uintptr_t message, uintptr_t kind, uintptr_t d) {
    if (!g_originalAdd) return;
    if (message == 0 || t_inside) {
        g_originalAdd(self, message, kind, d);
        return;
    }

    const crash::Breadcrumb breadcrumb("chat hook");

    const uintptr_t author = message + static_cast<uintptr_t>(g_authorOffset);
    const uintptr_t text = message + static_cast<uintptr_t>(g_textOffset);
    const Verdict verdict = hear(memory::readString(author), memory::readString(text));

    if (verdict.cancelled) {
        // Not drawing it means not passing it on, and then its destruction is ours:
        // the function it was meant for is the one that would have freed its strings.
        // Without the destructor the strings are left behind, a few dozen bytes,
        // rather than a line the player was promised they would not see.
        if (g_destroy) g_destroy(message);
        return;
    }

    if (verdict.rewritten) {
        // The message is the callee's to destroy, so new text has to come from the
        // heap the game frees into. Without the allocator the line goes out as it came.
        if (!memory::assignGameString(author, verdict.sender) &&
            !g_keptLine.exchange(true, std::memory_order_acq_rel)) {
            Log::warn(kLog, "a listener rewrote a line but the game's heap is out of reach; "
                            "the line goes out as it came");
        }
        if (!memory::assignGameString(text, verdict.message) &&
            !g_keptText.exchange(true, std::memory_order_acq_rel)) {
            Log::warn(kLog, "a listener rewrote a line but the game's heap is out of reach; "
                            "the line goes out as it came");
        }
    }

    g_originalAdd(self, message, kind, d);
}

}

ChatHook::ChatHook() : Hook("Chat", 0) {}

bool ChatHook::install() {
    // The shape the build has: the function every line ends in where the pack names it
    // and knows where a GuiMessage keeps its two strings, the older two-string display
    // call otherwise.
    if (const uintptr_t add = sig::address(hooks::kAddMessage); add != 0) {
        g_authorOffset = sig::offset(hooks::kMessageAuthor);
        g_textOffset = sig::offset(hooks::kMessageText);
        if (g_authorOffset < 0 || g_textOffset < 0) {
            Log::info(kLog, "{} is in the pack but {} and {} are not: nothing reads the chat",
                      hooks::kAddMessage, hooks::kMessageAuthor, hooks::kMessageText);
            return false;
        }

        g_destroy = reinterpret_cast<DestroyFn>(sig::address(hooks::kMessageDestroy));
        memory::setGameAllocator(sig::address(hooks::kAllocator));

        target_ = add;
        if (!create(reinterpret_cast<void*>(&addDetour), reinterpret_cast<void**>(&g_originalAdd))) {
            return false;
        }

        Log::info(kLog, "standing on {}{}{}", hooks::kAddMessage,
                  g_destroy ? "" : "; a dropped line leaks its strings",
                  memory::gameHeapReady() ? "" : "; lines cannot be rewritten");
        return true;
    }

    if (const uintptr_t display = sig::address(hooks::kDisplayChatMessage); display != 0) {
        target_ = display;
        if (!create(reinterpret_cast<void*>(&displayDetour),
                    reinterpret_cast<void**>(&g_originalDisplay))) {
            return false;
        }
        Log::info(kLog, "standing on {}", hooks::kDisplayChatMessage);
        return true;
    }

    Log::info(kLog, "neither {} nor {} in the pack: nothing reads the chat", hooks::kAddMessage,
              hooks::kDisplayChatMessage);
    return false;
}

}
