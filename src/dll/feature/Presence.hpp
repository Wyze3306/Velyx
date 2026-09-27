#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Color.hpp"
#include "dll/event/Events.hpp"

namespace velyx {

enum class PeerSource {

    // It said so, in chat, in the one line a Velyx client says when it arrives.
    Chat,

    // Another instance this launcher is running on this machine, found through a file
    // in %APPDATA%/Velyx rather than through the game.
    Instance,
};

const char* peerSourceLabel(PeerSource source);

struct Peer {
    std::string name;
    std::string version;
    PeerSource source = PeerSource::Chat;
    long long lastSeenMs = 0;
};

/// Who else here is running Velyx.
///
/// There is no directory to ask and nothing to sign in to: a client is only ever
/// known because it said so in chat, or because it is another instance of this
/// launcher on this machine. Both sources are optional, both are the user's choice,
/// and neither one reaches past the server you are already on.
///
/// Nothing in here decides what a badge looks like on screen — it holds the answer to
/// "is this name one of ours", and the chat and the nametags each draw it their own
/// way.
class Presence {
public:
    static Presence& get();

    void bind();

    // Set by the `velyx_users` module, which is the only thing that turns any of this
    // on. With the module off every one of them is false and the registry stays empty.
    // Atomic because they are set on the render thread and read on two others: the
    // game's, inside the chat handler, and the roster's.
    std::atomic<bool> listen{false};
    std::atomic<bool> announce{false};
    std::atomic<bool> answer{false};
    std::atomic<bool> instances{false};

    void setBadge(std::string text, std::string colour);

    [[nodiscard]] bool knows(std::string_view name) const;
    [[nodiscard]] std::vector<Peer> list() const;
    [[nodiscard]] size_t count() const;

    void mark(std::string name, std::string version, PeerSource source);
    void clear();

    // What goes in front of a name in the chat, formatting codes included, and the
    // same badge as a colour for whatever draws itself.
    [[nodiscard]] std::string chatTag() const;
    [[nodiscard]] std::string badgeText() const;
    [[nodiscard]] Color colour() const;

    // The single line one Velyx client says to another. Sent through the game's own
    // chat, so it costs a message and the server sees it like any other.
    [[nodiscard]] static std::string handshake();
    static bool readHandshake(std::string_view text, std::string* name, std::string* version);

    // Queued rather than sent: chat goes out on the render thread, and this is called
    // from a menu, from a keybind, or from the middle of the game's chat handler.
    void announceIn(float seconds);

    [[nodiscard]] bool announcedHere() const;

private:
    Presence() = default;

    void onFrame(FrameEvent& event);
    void onChat(ChatReceiveEvent& event);
    void onJoin(WorldJoinEvent& event);
    void onLeave(WorldLeaveEvent& event);
    void onClosing(GameClosingEvent& event);

    void rosterLoop(std::stop_token stop);
    void publishSelf();
    void readRoster();
    void removeSelf() const;

    void stopRoster();

    mutable std::mutex mutex_;

    std::vector<Peer> peers_;

    // Found by whichever thread heard it, handed to the render thread to be told
    // about: a notification built anywhere else would be a race with the frame.
    std::vector<Peer> pending_;

    std::string badge_ = "V";
    std::string colourName_ = "Green";

    std::string selfName_;
    std::string selfServer_;
    bool selfInGame_ = false;

    float announceIn_ = -1.f;
    bool announcedHere_ = false;

    std::jthread roster_;
};

inline Presence& presence() { return Presence::get(); }

void bindPresence();

}
