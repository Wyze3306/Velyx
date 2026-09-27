#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace velyx {

// What the connection is doing, measured rather than read out of the game. Nothing
// here needs a signature: the datagrams cross Winsock whoever compiled the game, and
// a Bedrock server answers an unconnected ping from anyone — that is how the server
// list in the game's own menu gets its numbers.
struct NetworkStats {
    bool connected = false;

    std::string serverAddress;
    uint16_t serverPort = 0;

    // Round trip to the server, from Velyx's own probe. Negative until one comes back.
    float ping = -1.f;
    float bestPing = -1.f;
    float worstPing = -1.f;

    // How far the round trip wanders from one probe to the next. A steady two hundred
    // is a long way away; a jumpy forty is a bad line, and only the second one is
    // worth doing anything about.
    float jitter = -1.f;

    // Of the probes sent, the share that went unanswered. Negative while the answer
    // would be a lie — see probeAnswered.
    float loss = -1.f;

    // False when the server has never answered a probe. A Realms world or an Xbox
    // relay is reached through a peer that is not a RakNet server, and silence from
    // it is not packet loss; it means the probe cannot measure this connection.
    bool probeAnswered = false;

    // Silences from the server long enough to be felt, counted over the last half
    // minute, and the worst of them. Measured without sending anything, so these are
    // the two numbers that survive a server which ignores probes.
    int stalls = 0;
    float worstGap = -1.f;

    float inboundPerSecond = 0.f;
    float outboundPerSecond = 0.f;
    float inboundKibPerSecond = 0.f;
    float outboundKibPerSecond = 0.f;

    int playersOnline = -1;
    int playerLimit = -1;
};

class NetworkMonitor {
public:
    static NetworkMonitor& get();

    // Counting starts when the first module asks for it and stops when the last one
    // lets go, so a client with the network modules off pays for a branch in the
    // detour and nothing else.
    void acquire();
    void release();
    [[nodiscard]] bool counting() const { return counters_.load(std::memory_order_relaxed) > 0; }

    // The cheap half of stats(), for the dozen relevance checks a frame that only want
    // to know whether there is a connection to describe at all.
    [[nodiscard]] bool connected() const { return connected_.load(std::memory_order_relaxed); }

    // Subscribed once, for the life of the client, like the other counters: poll()
    // costs a branch while nothing is asking, and a module turning on must not have
    // to also arrange for the numbers to be worked out.
    void bind();

    // The probe is counted the same way, because more than one readout wants it and
    // none of them owns it: the last one to let go is the one that stops it.
    void acquireProbe();
    void releaseProbe();

    void setProbeInterval(float seconds);

    // How long a silence from the server counts as one, in milliseconds.
    void setStallThreshold(float milliseconds);

    // Once a frame, on the thread that draws. Everything the detours leave behind is
    // turned into a rate or a window here, so that a dozen readouts asking the same
    // question in the same frame all get the same answer for the price of one.
    void poll(float deltaSeconds);

    [[nodiscard]] NetworkStats stats() const;

    // Both of these run inside the Winsock detours, on whichever thread the game does
    // its networking from, once per datagram. They take no lock and allocate nothing.
    void recordSent(const void* address, int addressLength, int bytes);
    void recordReceived(const void* address, int addressLength, int bytes);

    void shutdown();

private:
    NetworkMonitor() = default;

    void onFrame(struct FrameEvent& event);

    // A peer is a fixed slot rather than a map entry, so the detour finds it with a
    // short scan and never allocates. Eight is more than a Bedrock client uses: the
    // server, the LAN broadcast it shouts into, and room to spare.
    static constexpr size_t kPeerCount = 8;

    // Half a minute of stall history, in one-second buckets.
    static constexpr size_t kStallSeconds = 30;

    static constexpr size_t kPingSamples = 30;

    struct Peer {
        // Claimed first, then filled, then published. A reader that sees ready is
        // guaranteed to see the address that goes with it.
        std::atomic<bool> claimed{false};
        std::atomic<bool> ready{false};

        uint16_t family = 0;
        uint16_t port = 0;
        int addressLength = 0;
        std::array<uint8_t, 16> address{};

        std::atomic<uint64_t> sent{0};
        std::atomic<uint64_t> received{0};
        std::atomic<uint64_t> sentBytes{0};
        std::atomic<uint64_t> receivedBytes{0};

        std::atomic<long long> lastReceivedTicks{0};

        // Cumulative since the slot was claimed; the window is worked out by the
        // reader, so the detour only ever adds.
        std::atomic<uint32_t> stalls{0};
        std::atomic<uint32_t> worstGapMicros{0};
    };

    Peer* findOrClaim(const void* address, int addressLength);
    [[nodiscard]] Peer* busiest();

    void probeLoop(const std::stop_token& stop);

    // The thread outlives any one readout that wants it. Stopping it means joining
    // it, and joining it means waiting out whatever read it is sitting in — which is
    // not something a module being switched off on the render thread may do. So it
    // starts once, idles on a condition while nothing is asking, and is only ever
    // joined on the way out of the process.
    void startProbe();
    void stopProbe();
    void forgetProbe();
    [[nodiscard]] bool waitForWork(const std::stop_token& stop);

    std::array<Peer, kPeerCount> peers_{};
    std::atomic<int> counters_{0};

    std::atomic<int> probeCounters_{0};
    std::atomic<float> probeInterval_{1.f};
    std::atomic<uint32_t> stallThresholdMicros_{200'000};

    std::jthread prober_;
    std::mutex idleMutex_;
    std::condition_variable idle_;

    // Written by the probe thread, read by poll().
    mutable std::mutex probeMutex_;
    std::array<float, kPingSamples> pings_{};
    size_t pingCount_ = 0;
    size_t pingNext_ = 0;
    int probesSent_ = 0;
    int probesAnswered_ = 0;
    bool everAnswered_ = false;
    int playersOnline_ = -1;
    int playerLimit_ = -1;

    // poll()'s own state: the previous totals the rates are differences of, and the
    // stall buckets.
    Peer* watched_ = nullptr;
    long long lastPollTicks_ = 0;
    uint64_t lastSent_ = 0;
    uint64_t lastReceived_ = 0;
    uint64_t lastSentBytes_ = 0;
    uint64_t lastReceivedBytes_ = 0;
    uint32_t lastStalls_ = 0;
    std::array<uint32_t, kStallSeconds> stallBuckets_{};
    size_t stallBucket_ = 0;
    float bucketSeconds_ = 0.f;

    mutable std::mutex statsMutex_;
    NetworkStats current_;
    std::atomic<bool> connected_{false};
};

}
