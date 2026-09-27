#include "NetworkMonitor.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>

#include "core/Log.hpp"
#include "dll/event/EventBus.hpp"
#include "dll/event/Events.hpp"

namespace velyx {
namespace {

constexpr const char* kLog = "Network";

// The sixteen bytes RakNet puts in every message that travels outside a session, so
// that a stray datagram on the same port is not mistaken for one of its own.
constexpr uint8_t kOfflineMagic[16] = {0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE, 0xFE,
                                       0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56, 0x78};

constexpr uint8_t kUnconnectedPing = 0x01;
constexpr uint8_t kUnconnectedPong = 0x1C;

// How long one read waits before the loop around it looks at the stop token again.
constexpr DWORD kReadTimeoutMs = 100;

long long ticks() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

long long tickRate() {
    static const long long rate = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? frequency.QuadPart : 1;
    }();
    return rate;
}

double microsBetween(long long from, long long to) {
    return static_cast<double>(to - from) / static_cast<double>(tickRate()) * 1.0e6;
}

void writeBigEndian64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

uint64_t readBigEndian64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
    return value;
}

struct Key {
    uint16_t family = 0;
    uint16_t port = 0;
    int length = 0;
    std::array<uint8_t, 16> address{};
};

// Everything a peer is identified by, pulled out of the sockaddr by hand. Comparing
// the sockaddrs themselves does not work: the padding a caller leaves in the one it
// hands to sendto is not the padding recvfrom writes into the one it fills.
bool keyFrom(const void* address, int addressLength, Key& key) {
    if (address == nullptr) return false;

    const auto* generic = static_cast<const sockaddr*>(address);

    if (generic->sa_family == AF_INET && addressLength >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(address);
        key.family = AF_INET;
        key.port = ntohs(in->sin_port);
        key.length = 4;
        std::memcpy(key.address.data(), &in->sin_addr, 4);

        // A broadcast or a multicast group is the client shouting for LAN games, not a
        // server it is talking to, and counting it would put the busiest peer in the
        // wrong place.
        const uint8_t first = key.address[0];
        if (first >= 224) return false;

        return key.port != 0;
    }

    if (generic->sa_family == AF_INET6 && addressLength >= static_cast<int>(sizeof(sockaddr_in6))) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        key.family = AF_INET6;
        key.port = ntohs(in6->sin6_port);
        key.length = 16;
        std::memcpy(key.address.data(), &in6->sin6_addr, 16);

        if (key.address[0] == 0xFF) return false;

        return key.port != 0;
    }

    return false;
}

std::string textOf(const Key& key) {
    char buffer[INET6_ADDRSTRLEN]{};
    if (inet_ntop(key.family, key.address.data(), buffer, sizeof(buffer)) == nullptr) return {};
    return buffer;
}

// The tail of an unconnected pong is the same semicolon-separated line the server list
// prints, and two of its fields are the player counts:
// MCPE;<motd>;<protocol>;<version>;<online>;<max>;...
void parseMotd(const char* text, size_t length, int& online, int& limit) {
    std::string_view view(text, length);

    size_t field = 0;
    size_t start = 0;
    while (start <= view.size() && field <= 5) {
        const size_t end = std::min(view.find(';', start), view.size());
        const std::string_view part = view.substr(start, end - start);

        if (field == 4 || field == 5) {
            int value = 0;
            const auto* first = part.data();
            const auto* last = first + part.size();
            if (std::from_chars(first, last, value).ec == std::errc{}) {
                (field == 4 ? online : limit) = value;
            }
        }

        start = end + 1;
        ++field;
    }
}

}

NetworkMonitor& NetworkMonitor::get() {
    static NetworkMonitor instance;
    return instance;
}

void NetworkMonitor::bind() {
    events().on<FrameEvent>(this, &NetworkMonitor::onFrame, EventPriority::First);
}

void NetworkMonitor::onFrame(FrameEvent& event) {
    if (!counting()) return;
    poll(event.deltaSeconds);
}

void NetworkMonitor::acquire() {
    if (counters_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        Log::info(kLog, "counting datagrams");
    }
}

void NetworkMonitor::release() {
    if (counters_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        stopProbe();
        Log::info(kLog, "no longer counting datagrams");
    }
}

void NetworkMonitor::acquireProbe() {
    probeCounters_.fetch_add(1, std::memory_order_acq_rel);
    startProbe();
    idle_.notify_all();
}

void NetworkMonitor::releaseProbe() {
    if (probeCounters_.fetch_sub(1, std::memory_order_acq_rel) == 1) forgetProbe();
    idle_.notify_all();
}

void NetworkMonitor::setProbeInterval(float seconds) {
    probeInterval_.store(std::clamp(seconds, 0.25f, 10.f), std::memory_order_relaxed);
}

void NetworkMonitor::setStallThreshold(float milliseconds) {
    stallThresholdMicros_.store(
        static_cast<uint32_t>(std::clamp(milliseconds, 50.f, 5000.f) * 1000.f),
        std::memory_order_relaxed);
}

NetworkMonitor::Peer* NetworkMonitor::findOrClaim(const void* address, int addressLength) {
    Key key;
    if (!keyFrom(address, addressLength, key)) return nullptr;

    for (Peer& peer : peers_) {
        if (!peer.ready.load(std::memory_order_acquire)) continue;
        if (peer.family != key.family || peer.port != key.port) continue;
        if (std::memcmp(peer.address.data(), key.address.data(),
                        static_cast<size_t>(key.length)) != 0) {
            continue;
        }
        return &peer;
    }

    for (Peer& peer : peers_) {
        bool expected = false;
        if (!peer.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            continue;
        }

        peer.family = key.family;
        peer.port = key.port;
        peer.addressLength = key.length;
        peer.address = key.address;
        peer.ready.store(true, std::memory_order_release);
        return &peer;
    }

    // Every slot is taken by something that is not this peer. Whatever it is, it is
    // the ninth thing the game is talking to, and it is not the server.
    return nullptr;
}

void NetworkMonitor::recordSent(const void* address, int addressLength, int bytes) {
    if (!counting() || bytes <= 0) return;

    Peer* peer = findOrClaim(address, addressLength);
    if (peer == nullptr) return;

    peer->sent.fetch_add(1, std::memory_order_relaxed);
    peer->sentBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);
}

void NetworkMonitor::recordReceived(const void* address, int addressLength, int bytes) {
    if (!counting() || bytes <= 0) return;

    Peer* peer = findOrClaim(address, addressLength);
    if (peer == nullptr) return;

    peer->received.fetch_add(1, std::memory_order_relaxed);
    peer->receivedBytes.fetch_add(static_cast<uint64_t>(bytes), std::memory_order_relaxed);

    const long long now = ticks();
    const long long previous = peer->lastReceivedTicks.exchange(now, std::memory_order_relaxed);
    if (previous == 0) return;

    const double gap = microsBetween(previous, now);
    if (gap <= 0.0) return;

    const auto micros = static_cast<uint32_t>(std::min(gap, 60.0e6));
    if (micros < stallThresholdMicros_.load(std::memory_order_relaxed)) return;

    peer->stalls.fetch_add(1, std::memory_order_relaxed);

    uint32_t worst = peer->worstGapMicros.load(std::memory_order_relaxed);
    while (micros > worst &&
           !peer->worstGapMicros.compare_exchange_weak(worst, micros, std::memory_order_relaxed)) {
    }
}

NetworkMonitor::Peer* NetworkMonitor::busiest() {
    Peer* best = nullptr;
    uint64_t most = 0;

    // What the client sends, not what it receives: a server sends far more than it is
    // sent, but so does anything else that happens to be talking, and only the server
    // is talked back to at a steady rate.
    for (Peer& peer : peers_) {
        if (!peer.ready.load(std::memory_order_acquire)) continue;

        const uint64_t sent = peer.sent.load(std::memory_order_relaxed);
        if (sent <= most) continue;

        most = sent;
        best = &peer;
    }

    return most >= 8 ? best : nullptr;
}

void NetworkMonitor::poll(float deltaSeconds) {
    NetworkStats next;

    Peer* peer = busiest();

    if (peer != watched_) {
        // A different peer is a different connection, and none of the previous totals
        // mean anything against it.
        watched_ = peer;
        lastPollTicks_ = 0;
        lastSent_ = lastReceived_ = lastSentBytes_ = lastReceivedBytes_ = 0;
        lastStalls_ = 0;
        stallBuckets_.fill(0);
        stallBucket_ = 0;
        bucketSeconds_ = 0.f;
        forgetProbe();
    }

    if (peer != nullptr) {
        Key key;
        key.family = peer->family;
        key.port = peer->port;
        key.length = peer->addressLength;
        key.address = peer->address;

        next.connected = true;
        next.serverAddress = textOf(key);
        next.serverPort = peer->port;

        const uint64_t sent = peer->sent.load(std::memory_order_relaxed);
        const uint64_t received = peer->received.load(std::memory_order_relaxed);
        const uint64_t sentBytes = peer->sentBytes.load(std::memory_order_relaxed);
        const uint64_t receivedBytes = peer->receivedBytes.load(std::memory_order_relaxed);

        const long long now = ticks();
        const bool first = lastPollTicks_ == 0;
        const double elapsed = first ? 0.0 : microsBetween(lastPollTicks_, now) / 1.0e6;

        if (elapsed > 0.2) {
            const auto rate = [elapsed](uint64_t from, uint64_t to) {
                return to >= from ? static_cast<float>(static_cast<double>(to - from) / elapsed)
                                  : 0.f;
            };

            next.outboundPerSecond = rate(lastSent_, sent);
            next.inboundPerSecond = rate(lastReceived_, received);
            next.outboundKibPerSecond = rate(lastSentBytes_, sentBytes) / 1024.f;
            next.inboundKibPerSecond = rate(lastReceivedBytes_, receivedBytes) / 1024.f;

            lastPollTicks_ = now;
            lastSent_ = sent;
            lastReceived_ = received;
            lastSentBytes_ = sentBytes;
            lastReceivedBytes_ = receivedBytes;
        } else if (first) {
            lastPollTicks_ = now;
            lastSent_ = sent;
            lastReceived_ = received;
            lastSentBytes_ = sentBytes;
            lastReceivedBytes_ = receivedBytes;
        } else {
            const std::lock_guard<std::mutex> guard(statsMutex_);
            next.inboundPerSecond = current_.inboundPerSecond;
            next.outboundPerSecond = current_.outboundPerSecond;
            next.inboundKibPerSecond = current_.inboundKibPerSecond;
            next.outboundKibPerSecond = current_.outboundKibPerSecond;
        }

        // The stall count is cumulative, so the window is a ring of one-second buckets
        // and what is reported is their sum.
        const uint32_t stalls = peer->stalls.load(std::memory_order_relaxed);
        if (first) {
            // Whatever this peer had stalled before anyone was watching belongs to
            // nobody's window.
            lastStalls_ = stalls;
        }
        stallBuckets_[stallBucket_] += stalls - lastStalls_;
        lastStalls_ = stalls;

        bucketSeconds_ += deltaSeconds;
        while (bucketSeconds_ >= 1.f) {
            bucketSeconds_ -= 1.f;
            stallBucket_ = (stallBucket_ + 1) % kStallSeconds;
            stallBuckets_[stallBucket_] = 0;
        }

        uint32_t total = 0;
        for (const uint32_t bucket : stallBuckets_) total += bucket;
        next.stalls = static_cast<int>(total);

        // The two numbers are read together, so they have to describe the same window:
        // once the last stall has aged out of it, the worst one goes with it.
        if (total == 0) peer->worstGapMicros.store(0, std::memory_order_relaxed);

        const uint32_t worst = peer->worstGapMicros.load(std::memory_order_relaxed);
        next.worstGap = worst == 0 ? -1.f : static_cast<float>(worst) / 1000.f;
    }

    {
        const std::lock_guard<std::mutex> guard(probeMutex_);

        next.probeAnswered = everAnswered_;
        next.playersOnline = playersOnline_;
        next.playerLimit = playerLimit_;

        if (pingCount_ > 0) {
            float sum = 0.f;
            float best = pings_[0];
            float worst = pings_[0];
            for (size_t i = 0; i < pingCount_; ++i) {
                sum += pings_[i];
                best = std::min(best, pings_[i]);
                worst = std::max(worst, pings_[i]);
            }

            const float mean = sum / static_cast<float>(pingCount_);

            float deviation = 0.f;
            for (size_t i = 0; i < pingCount_; ++i) deviation += std::fabs(pings_[i] - mean);

            next.ping = mean;
            next.bestPing = best;
            next.worstPing = worst;
            next.jitter = deviation / static_cast<float>(pingCount_);
        }

        // Loss is only an honest number once the server has proved it answers at all.
        if (everAnswered_ && probesSent_ > 0) {
            next.loss = 1.f - static_cast<float>(probesAnswered_) / static_cast<float>(probesSent_);
            next.loss = std::clamp(next.loss, 0.f, 1.f);
        }
    }

    connected_.store(next.connected, std::memory_order_relaxed);

    const std::lock_guard<std::mutex> guard(statsMutex_);
    current_ = std::move(next);
}

NetworkStats NetworkMonitor::stats() const {
    const std::lock_guard<std::mutex> guard(statsMutex_);
    return current_;
}

void NetworkMonitor::forgetProbe() {
    const std::lock_guard<std::mutex> guard(probeMutex_);
    pingCount_ = 0;
    pingNext_ = 0;
    probesSent_ = 0;
    probesAnswered_ = 0;
    everAnswered_ = false;
    playersOnline_ = -1;
    playerLimit_ = -1;
}

void NetworkMonitor::startProbe() {
    if (prober_.joinable()) return;
    prober_ = std::jthread([this](std::stop_token stop) { probeLoop(stop); });
}

void NetworkMonitor::stopProbe() {
    if (!prober_.joinable()) return;

    prober_.request_stop();
    idle_.notify_all();
    prober_.join();
}

// Returns false only when the thread is being wound up for good. Anything else is a
// wait: nothing is asking for a probe, so nothing is sent.
bool NetworkMonitor::waitForWork(const std::stop_token& stop) {
    std::unique_lock<std::mutex> lock(idleMutex_);
    idle_.wait(lock, [&] {
        return stop.stop_requested() || probeCounters_.load(std::memory_order_relaxed) > 0;
    });
    return !stop.stop_requested();
}

// One socket of Velyx's own, one datagram a second, and the same unconnected ping the
// server list sends when it draws a row. It is not the game's session: a server that
// answers it answers everyone, and nothing here touches the connection being measured.
void NetworkMonitor::probeLoop(const std::stop_token& stop) {
    WSADATA winsock{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;

    SOCKET probe = INVALID_SOCKET;
    uint16_t socketFamily = 0;

    const auto close = [&] {
        if (probe == INVALID_SOCKET) return;
        closesocket(probe);
        probe = INVALID_SOCKET;
        socketFamily = 0;
    };

    const uint64_t guid = static_cast<uint64_t>(ticks()) ^ 0x56'65'6C'79'78'00'00'01ULL;

    while (!stop.stop_requested()) {
        if (probeCounters_.load(std::memory_order_relaxed) <= 0) {
            close();
            if (!waitForWork(stop)) break;
            continue;
        }

        const float interval = std::clamp(probeInterval_.load(std::memory_order_relaxed), 0.25f,
                                          10.f);

        Peer* peer = busiest();
        if (peer == nullptr) {
            Sleep(kReadTimeoutMs);
            continue;
        }

        if (probe == INVALID_SOCKET || socketFamily != peer->family) {
            close();

            socketFamily = peer->family;
            probe = socket(socketFamily, SOCK_DGRAM, IPPROTO_UDP);
            if (probe == INVALID_SOCKET) {
                Log::warn(kLog, "no socket for the probe ({}), it stays off", WSAGetLastError());
                if (!waitForWork(stop)) break;
                continue;
            }

            // Short on purpose. The wait for an answer is a loop around this rather
            // than one long read, so that a stop request is never more than a tenth of
            // a second away from being noticed.
            DWORD timeout = kReadTimeoutMs;
            setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout));
        }

        sockaddr_storage target{};
        int targetLength = 0;
        if (peer->family == AF_INET) {
            auto* in = reinterpret_cast<sockaddr_in*>(&target);
            in->sin_family = AF_INET;
            in->sin_port = htons(peer->port);
            std::memcpy(&in->sin_addr, peer->address.data(), 4);
            targetLength = sizeof(sockaddr_in);
        } else {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&target);
            in6->sin6_family = AF_INET6;
            in6->sin6_port = htons(peer->port);
            std::memcpy(&in6->sin6_addr, peer->address.data(), 16);
            targetLength = sizeof(sockaddr_in6);
        }

        const long long sentAt = ticks();
        const auto stamp = static_cast<uint64_t>(sentAt);

        uint8_t request[33]{};
        request[0] = kUnconnectedPing;
        writeBigEndian64(request + 1, stamp);
        std::memcpy(request + 9, kOfflineMagic, sizeof(kOfflineMagic));
        writeBigEndian64(request + 25, guid);

        const int written = sendto(probe, reinterpret_cast<const char*>(request), sizeof(request),
                                   0, reinterpret_cast<const sockaddr*>(&target), targetLength);
        if (written != static_cast<int>(sizeof(request))) {
            Sleep(kReadTimeoutMs);
            continue;
        }

        {
            const std::lock_guard<std::mutex> guard(probeMutex_);
            ++probesSent_;

            // The share is over a window rather than over the session: a bad minute an
            // hour ago is not what the player is being told about.
            if (probesSent_ > 120) {
                probesSent_ /= 2;
                probesAnswered_ /= 2;
            }
        }

        // The answer has until the next probe is due. Anything else arriving on this
        // socket in the meantime is not it, and is read past rather than counted.
        const double deadlineMicros = static_cast<double>(interval) * 1.0e6;
        while (!stop.stop_requested() && microsBetween(sentAt, ticks()) < deadlineMicros) {
            uint8_t reply[1024]{};
            sockaddr_storage from{};
            int fromLength = sizeof(from);

            const int read = recvfrom(probe, reinterpret_cast<char*>(reply), sizeof(reply), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (read < 35) continue;
            if (reply[0] != kUnconnectedPong) continue;
            if (readBigEndian64(reply + 1) != stamp) continue;
            if (std::memcmp(reply + 17, kOfflineMagic, sizeof(kOfflineMagic)) != 0) continue;

            const auto round = static_cast<float>(microsBetween(sentAt, ticks()) / 1000.0);

            int online = -1;
            int limit = -1;
            const auto length = static_cast<size_t>((reply[33] << 8) | reply[34]);
            if (length > 0 && 35 + length <= static_cast<size_t>(read)) {
                parseMotd(reinterpret_cast<const char*>(reply + 35), length, online, limit);
            }

            const std::lock_guard<std::mutex> guard(probeMutex_);
            pings_[pingNext_] = round;
            pingNext_ = (pingNext_ + 1) % kPingSamples;
            pingCount_ = std::min(pingCount_ + 1, kPingSamples);

            ++probesAnswered_;
            everAnswered_ = true;
            playersOnline_ = online;
            playerLimit_ = limit;
            break;
        }

        // Whatever is left of the interval after waiting for the answer, in slices: a
        // five-second gap between probes must not be five seconds between a stop
        // request and the thread noticing it.
        for (;;) {
            const double left = static_cast<double>(interval) * 1000.0 -
                                microsBetween(sentAt, ticks()) / 1000.0;
            if (left <= 0.0 || stop.stop_requested()) break;
            if (probeCounters_.load(std::memory_order_relaxed) <= 0) break;

            Sleep(std::min<DWORD>(static_cast<DWORD>(left), kReadTimeoutMs));
        }
    }

    close();
    if (started) WSACleanup();
}

void NetworkMonitor::shutdown() {
    stopProbe();
    probeCounters_.store(0, std::memory_order_relaxed);
    counters_.store(0, std::memory_order_relaxed);
}

}
