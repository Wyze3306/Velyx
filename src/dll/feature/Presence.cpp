#include "Presence.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <format>

#include <json/json.hpp>
#include <velyx/Version.hpp>

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "core/Strings.hpp"
#include "dll/sdk/Game.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Presence";

// Short, plain and strictly shaped. Plain because a server that filters exotic
// characters would otherwise silently break the handshake; strictly shaped because a
// player typing the marker by hand should not be mistaken for a client saying it.
//
//     [Velyx] <version> <name>
//
// The name travels in the line because most servers rewrite chat into a single
// string and leave the sender empty, and a badge on nobody is worth nothing.
constexpr std::string_view kMarker = "[Velyx]";

// A peer heard in chat is heard once and is good for the rest of the world; an
// instance on this machine has to keep saying so, because the only way to know it
// went away is that it stopped.
constexpr long long kInstanceTimeoutMs = 15'000;
constexpr long long kRosterStaleMs = 5 * 60 * 1000;
constexpr auto kRosterPeriod = std::chrono::milliseconds(3000);
constexpr auto kRosterSlice = std::chrono::milliseconds(250);

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::filesystem::path rosterDirectory() { return Paths::cache() / "presence"; }

std::filesystem::path rosterFile() {
    return rosterDirectory() / std::format("{}.json", GetCurrentProcessId());
}

bool sameName(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

bool looksLikeVersion(std::string_view text) {
    if (text.empty() || text.size() > 32) return false;

    int dots = 0;
    for (const char c : text) {
        if (c == '.') ++dots;
        else if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '+') return false;
    }
    return dots >= 1;
}

struct BadgeColour {
    const char* name;
    char code;
    Color colour;
};

// The five Minecraft colours a badge can be, so the chip over a head and the tag in
// the chat are the same colour rather than two takes on it.
const BadgeColour kColours[] = {
    {"Green", 'a', Color::rgb8(85, 255, 85)},
    {"Aqua", 'b', Color::rgb8(85, 255, 255)},
    {"Gold", '6', Color::rgb8(255, 170, 0)},
    {"Pink", 'd', Color::rgb8(255, 85, 255)},
    {"Grey", '7', Color::rgb8(170, 170, 170)},
};

const BadgeColour& badgeColour(std::string_view name) {
    for (const BadgeColour& entry : kColours) {
        if (entry.name == name) return entry;
    }
    return kColours[0];
}

}

const char* peerSourceLabel(PeerSource source) {
    switch (source) {
        case PeerSource::Chat:     return "Chat";
        case PeerSource::Instance: return "This machine";
    }
    return "Unknown";
}

Presence& Presence::get() {
    static Presence instance;
    return instance;
}

std::string Presence::handshake() {
    const std::string& name = sdk::game().player().name;
    return std::format("{} {} {}", kMarker, version::kString, name.empty() ? "?" : name);
}

bool Presence::readHandshake(std::string_view text, std::string* name, std::string* version) {
    const std::string plain = strings::stripFormatting(text);

    // Anywhere in the line, not only at the front: most servers wrap what a player
    // says in a rank, a name and whatever punctuation they favour this week.
    const size_t marker = plain.find(kMarker);
    if (marker == std::string::npos) return false;

    std::string_view line = strings::trim(std::string_view(plain).substr(marker + kMarker.size()));
    const size_t space = line.find(' ');
    if (space == std::string_view::npos) return false;

    const std::string_view saidVersion = line.substr(0, space);
    const std::string_view saidName = strings::trim(line.substr(space + 1));

    if (!looksLikeVersion(saidVersion) || saidName.empty() || saidName.size() > 64) return false;

    if (version) *version = std::string(saidVersion);
    if (name) *name = std::string(saidName);
    return true;
}

bool Presence::knows(std::string_view name) const {
    if (name.empty()) return false;

    const std::lock_guard lock(mutex_);
    return std::any_of(peers_.begin(), peers_.end(),
                       [&](const Peer& peer) { return sameName(peer.name, name); });
}

std::vector<Peer> Presence::list() const {
    const std::lock_guard lock(mutex_);
    return peers_;
}

size_t Presence::count() const {
    const std::lock_guard lock(mutex_);
    return peers_.size();
}

void Presence::mark(std::string name, std::string version, PeerSource source) {
    name = strings::stripFormatting(name);
    if (name.empty()) return;

    const std::lock_guard lock(mutex_);

    if (sameName(name, selfName_)) return;

    for (Peer& peer : peers_) {
        if (!sameName(peer.name, name)) continue;

        peer.lastSeenMs = nowMs();
        if (!version.empty()) peer.version = std::move(version);

        // Hearing it in chat is worth more than finding it in a file: it means the
        // client is here, in this world, rather than merely running on this machine.
        if (source == PeerSource::Chat) peer.source = source;
        return;
    }

    Peer peer;
    peer.name = std::move(name);
    peer.version = std::move(version);
    peer.source = source;
    peer.lastSeenMs = nowMs();

    peers_.push_back(peer);
    pending_.push_back(std::move(peer));
}

void Presence::clear() {
    const std::lock_guard lock(mutex_);
    peers_.clear();
    pending_.clear();
}

void Presence::setBadge(std::string text, std::string colour) {
    const std::lock_guard lock(mutex_);
    badge_ = std::move(text);
    colourName_ = std::move(colour);
}

std::string Presence::badgeText() const {
    const std::lock_guard lock(mutex_);
    return badge_;
}

std::string Presence::chatTag() const {
    const std::lock_guard lock(mutex_);
    if (badge_.empty()) return {};
    return std::format("\u00a7{}[{}]\u00a7r", badgeColour(colourName_).code, badge_);
}

Color Presence::colour() const {
    const std::lock_guard lock(mutex_);
    return badgeColour(colourName_).colour;
}

void Presence::announceIn(float seconds) {
    const std::lock_guard lock(mutex_);
    if (announceIn_ >= 0.f) return;
    announceIn_ = std::max(0.f, seconds);
}

bool Presence::announcedHere() const {
    const std::lock_guard lock(mutex_);
    return announcedHere_;
}

void Presence::onJoin(WorldJoinEvent& event) {
    {
        const std::lock_guard lock(mutex_);
        peers_.clear();
        pending_.clear();
        announcedHere_ = false;
        announceIn_ = -1.f;
    }

    // Not straight away: the server is still sending join messages, and a line that
    // lands in the middle of them is a line nobody reads.
    if (announce && event.multiplayer) announceIn(6.f);
}

void Presence::onLeave(WorldLeaveEvent&) {
    const std::lock_guard lock(mutex_);
    peers_.clear();
    pending_.clear();
    announcedHere_ = false;
    announceIn_ = -1.f;
}

void Presence::onChat(ChatReceiveEvent& event) {
    if (!listen) return;

    std::string name;
    std::string version;
    if (!readHandshake(event.rawMessage, &name, &version)) return;

    // Whoever the server says sent it wins over whoever the line claims to be, when
    // the server bothers to say.
    const std::string sender = strings::stripFormatting(event.sender);
    if (!sender.empty()) name = sender;

    // Nobody sees the handshake, ours included. It is a client talking to a client,
    // and a chat line is only the envelope it travels in.
    event.cancel();

    bool self = false;
    {
        const std::lock_guard lock(mutex_);
        self = sameName(name, selfName_);
    }
    if (self) return;

    mark(name, version, PeerSource::Chat);

    // Answering is what makes one announcement enough for everybody: whoever arrives
    // says it, and whoever was already here says it back, once.
    if (answer && !announcedHere()) announceIn(1.5f);
}

void Presence::onFrame(FrameEvent& event) {
    const sdk::PlayerState& player = sdk::game().player();
    const sdk::WorldState& world = sdk::game().world();

    std::vector<Peer> found;
    bool sendNow = false;

    {
        const std::lock_guard lock(mutex_);

        selfName_ = player.name;
        selfServer_ = world.serverAddress;
        selfInGame_ = world.inGame;

        const long long now = nowMs();
        std::erase_if(peers_, [&](const Peer& peer) {
            return peer.source == PeerSource::Instance &&
                   now - peer.lastSeenMs > kInstanceTimeoutMs;
        });

        found.swap(pending_);

        if (announceIn_ >= 0.f) {
            announceIn_ -= event.deltaSeconds;
            if (announceIn_ <= 0.f) {
                announceIn_ = -1.f;
                sendNow = true;
            }
        }
    }

    for (const Peer& peer : found) {
        PeerFoundEvent notice;
        notice.name = peer.name;
        notice.version = peer.version;
        notice.local = peer.source == PeerSource::Instance;
        events().emit(notice);
    }

    if (!sendNow) return;
    if (!world.inGame || !world.multiplayer) return;

    if (sdk::game().sendChat(handshake())) {
        const std::lock_guard lock(mutex_);
        announcedHere_ = true;
        Log::debug(kLog, "announced on {}", world.serverAddress);
    }
}

void Presence::onClosing(GameClosingEvent&) { stopRoster(); }

// The roster: one small file per running client, rewritten every few seconds, read by
// every other one. It is the only source that works with no signature pack at all,
// and the only one that costs the server nothing — which is why it is also the one
// that can only ever see the instances this launcher started here.
void Presence::publishSelf() {
    std::string name;
    std::string server;
    bool inGame = false;

    {
        const std::lock_guard lock(mutex_);
        name = selfName_;
        server = selfServer_;
        inGame = selfInGame_;
    }

    if (!inGame || name.empty()) {
        removeSelf();
        return;
    }

    nlohmann::json entry;
    entry["name"] = name;
    entry["server"] = server;
    entry["version"] = version::kString;
    entry["pid"] = static_cast<int64_t>(GetCurrentProcessId());
    entry["time"] = nowMs();

    std::error_code ec;
    std::filesystem::create_directories(rosterDirectory(), ec);

    std::ofstream file(rosterFile(), std::ios::trunc);
    if (file) file << entry.dump();
}

void Presence::readRoster() {
    std::string server;
    {
        const std::lock_guard lock(mutex_);
        server = selfServer_;
    }

    const auto ours = rosterFile();
    const long long now = nowMs();

    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(rosterDirectory(), ec)) {
        if (ec) return;
        if (!file.is_regular_file() || file.path() == ours) continue;

        std::ifstream stream(file.path());
        if (!stream) continue;

        nlohmann::json entry = nlohmann::json::parse(stream, nullptr, false);
        if (entry.is_discarded() || !entry.is_object()) continue;

        const long long time = entry.value("time", 0LL);

        // A client that crashed leaves its file behind; nothing else ever tidies it.
        if (now - time > kRosterStaleMs) {
            std::error_code removeError;
            std::filesystem::remove(file.path(), removeError);
            continue;
        }

        if (now - time > kInstanceTimeoutMs) continue;
        if (entry.value("server", std::string{}) != server) continue;

        mark(entry.value("name", std::string{}), entry.value("version", std::string{}),
             PeerSource::Instance);
    }
}

void Presence::removeSelf() const {
    std::error_code ec;
    std::filesystem::remove(rosterFile(), ec);
}

void Presence::rosterLoop(std::stop_token stop) {
    auto sinceTick = kRosterPeriod;

    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(kRosterSlice);
        sinceTick += kRosterSlice;

        if (sinceTick < kRosterPeriod) continue;
        sinceTick = std::chrono::milliseconds(0);

        if (!instances) {
            removeSelf();
            continue;
        }

        try {
            publishSelf();
            readRoster();
        } catch (const std::exception& error) {
            Log::warn(kLog, "the roster could not be read: {}", error.what());
        } catch (...) {
            Log::warn(kLog, "the roster could not be read");
        }
    }

    removeSelf();
}

void Presence::stopRoster() {
    if (!roster_.joinable()) return;

    roster_.request_stop();
    roster_.join();
}

void Presence::bind() {
    events().on<FrameEvent>(this, &Presence::onFrame, EventPriority::High);
    events().on<ChatReceiveEvent>(this, &Presence::onChat, EventPriority::First);
    events().on<WorldJoinEvent>(this, &Presence::onJoin);
    events().on<WorldLeaveEvent>(this, &Presence::onLeave);
    events().on<GameClosingEvent>(this, &Presence::onClosing, EventPriority::First);

    roster_ = std::jthread([this](std::stop_token stop) { rosterLoop(std::move(stop)); });
}

void bindPresence() {
    Presence::get().bind();
    Log::debug(kLog, "presence bound");
}

}
