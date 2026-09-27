#include "NetworkHook.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <MinHook.h>

#include "core/Log.hpp"
#include "dll/feature/NetworkMonitor.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "NetworkHook";

using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WsaSendToFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
                                 LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WsaRecvFromFn = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT,
                                   LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

SendToFn g_originalSendTo = nullptr;
RecvFromFn g_originalRecvFrom = nullptr;
WsaSendToFn g_originalWsaSendTo = nullptr;
WsaRecvFromFn g_originalWsaRecvFrom = nullptr;

// These four run on the game's own network thread, once per datagram, in front of the
// call the game is waiting on. Everything they do is a handful of atomic adds behind
// a flag that is false until a module asks for the numbers; nothing here allocates,
// takes a lock, or can throw.
int WSAAPI sendToDetour(SOCKET socket, const char* buffer, int length, int flags,
                        const sockaddr* to, int toLength) {
    const int written = g_originalSendTo(socket, buffer, length, flags, to, toLength);
    if (written > 0) NetworkMonitor::get().recordSent(to, toLength, written);
    return written;
}

int WSAAPI recvFromDetour(SOCKET socket, char* buffer, int length, int flags, sockaddr* from,
                          int* fromLength) {
    const int read = g_originalRecvFrom(socket, buffer, length, flags, from, fromLength);
    if (read > 0 && fromLength != nullptr) {
        NetworkMonitor::get().recordReceived(from, *fromLength, read);
    }
    return read;
}

int WSAAPI wsaSendToDetour(SOCKET socket, LPWSABUF buffers, DWORD bufferCount, LPDWORD sent,
                           DWORD flags, const sockaddr* to, int toLength,
                           LPWSAOVERLAPPED overlapped,
                           LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    const int result = g_originalWsaSendTo(socket, buffers, bufferCount, sent, flags, to, toLength,
                                           overlapped, completion);

    // An overlapped send that has not finished has no byte count yet, and the
    // completion arrives somewhere this detour cannot see. Counting it as zero would
    // be wrong, so it is not counted at all.
    if (result == 0 && sent != nullptr && *sent > 0) {
        NetworkMonitor::get().recordSent(to, toLength, static_cast<int>(*sent));
    }
    return result;
}

int WSAAPI wsaRecvFromDetour(SOCKET socket, LPWSABUF buffers, DWORD bufferCount, LPDWORD received,
                             LPDWORD flags, sockaddr* from, LPINT fromLength,
                             LPWSAOVERLAPPED overlapped,
                             LPWSAOVERLAPPED_COMPLETION_ROUTINE completion) {
    const int result = g_originalWsaRecvFrom(socket, buffers, bufferCount, received, flags, from,
                                             fromLength, overlapped, completion);

    if (result == 0 && received != nullptr && *received > 0 && fromLength != nullptr) {
        NetworkMonitor::get().recordReceived(from, *fromLength, static_cast<int>(*received));
    }
    return result;
}

}

NetworkHook::NetworkHook() : Hook("Network", 0) {}

bool NetworkHook::install() {
    // ws2_32 is already loaded — the game cannot have reached a server without it —
    // but a client running with no networking at all should cost the readouts, not
    // the injection.
    const HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
    if (winsock == nullptr) {
        Log::warn(kLog, "ws2_32 is not loaded, the network readouts stay empty");
        return false;
    }

    struct Entry {
        const char* symbol;
        void* detour;
        void** original;
    };

    const Entry entries[] = {
        {"sendto", reinterpret_cast<void*>(&sendToDetour),
         reinterpret_cast<void**>(&g_originalSendTo)},
        {"recvfrom", reinterpret_cast<void*>(&recvFromDetour),
         reinterpret_cast<void**>(&g_originalRecvFrom)},
        {"WSASendTo", reinterpret_cast<void*>(&wsaSendToDetour),
         reinterpret_cast<void**>(&g_originalWsaSendTo)},
        {"WSARecvFrom", reinterpret_cast<void*>(&wsaRecvFromDetour),
         reinterpret_cast<void**>(&g_originalWsaRecvFrom)},
    };

    size_t index = 0;
    int standing = 0;

    for (const Entry& entry : entries) {
        void* address = reinterpret_cast<void*>(GetProcAddress(winsock, entry.symbol));
        if (address == nullptr) {
            Log::warn(kLog, "ws2_32 has no {}", entry.symbol);
            ++index;
            continue;
        }

        if (createAt(address, entry.detour, entry.original)) {
            targets_[index] = address;
            ++standing;
        }
        ++index;
    }

    // Either half of the pair is enough to see the server; both halves failing means
    // there is nothing to measure and the hook says so rather than pretending.
    installed_ = standing > 0;
    Log::info(kLog, "{}/4 winsock entries hooked", standing);
    return installed_;
}

void NetworkHook::uninstall() {
    for (void*& target : targets_) {
        if (target == nullptr) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }

    installed_ = false;
    NetworkMonitor::get().shutdown();
}

}
