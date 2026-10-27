// One TLS connection (a client's, or the world server's chat link): framing, frame/rate limits, handshake deadline, heartbeat, batched writes and the lag
// simulator. Knows nothing about game rules; decoded messages go to on_message. All calls happen on the io thread.
#pragma once
#include "../core/clock.h"
#include "../core/rate_limiter.h"

#include <codec.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace kast::net {

struct SessionLimits {
    uint32_t maxFrameBytes = 4096;
    Millis handshakeTimeout{10000};
    Millis idleTimeout{60000};
    Millis pingInterval{20000};
    double msgRate = 60, msgBurst = 120;
    Millis msgRateGrace{2000};
    size_t maxSendBuffer = 1 << 20;
    Millis simLatency{0}, simJitter{0}; // lag simulator (SIM_LATENCY_MS / SIM_JITTER_MS): both 0 = off
};

// The lag simulator's delivery time for a message that came (or was sent) at now: latency +- jitter, uniform, but
// never before the previous message's (order is kept) nor before now.
inline TimePoint lag_due(TimePoint now, TimePoint previous, Millis latency, Millis jitter, std::minstd_rand& rng) {
    const auto j = jitter.count();
    const Millis offset{j ? std::uniform_int_distribution<long long>(-j, j)(rng) : 0};
    return std::max({now, previous, now + latency + offset});
}

// Frames Session::send and send_many have encoded, ever: lets a check prove a fan-out encodes once.
inline std::atomic<size_t> framesEncoded{0};
// Reads Session has issued, ever: lets a check prove that frames arriving together share one read.
inline std::atomic<size_t> readsIssued{0};

class Session : public std::enable_shared_from_this<Session> {
public:
    using OnMessage = std::function<void(Session&, proto::Message&&)>;
    using OnClosed = std::function<void(Session&)>;

    // role client: this end connects (the world server's chat link); the TLS handshake runs as the client and the
    // context does the peer verification.
    Session(uint32_t id, asio::ip::tcp::socket socket, asio::ssl::context& tls, const SessionLimits& limits,
            OnMessage onMessage, OnClosed onClosed,
            asio::ssl::stream_base::handshake_type role = asio::ssl::stream_base::server);

    // rejectWith: complete TLS, send Disconnect{rejectWith}, close (e.g. server_full).
    void start(std::optional<proto::Code> rejectWith = std::nullopt);
    void send(const proto::Message& m);
    // An already encoded frame (send_many): same rules as send (dropped while closing, lag simulator, buffer cap).
    void send_frame(std::span<const uint8_t> frame);
    // Sends Disconnect{reason}, stops reading, closes once it is written (or after a short grace).
    void disconnect(proto::Code reason);
    // Handshake finished (AuthOk sent): the handshake deadline no longer applies.
    void authenticated();

    uint32_t id() const { return id_; }
    bool closing() const { return closing_; }
    std::string remote() const { return remote_; }
    std::span<const uint8_t> pending() const { return pending_; } // queued for the next write; for tests

private:
    // Received messages wait decoded (for deliver), sent ones encoded (for pending_): a fan-out is not re-encoded.
    template <class T> struct Delayed {
        TimePoint due;
        T item;
    };
    template <class T> using DelayQueue = std::deque<Delayed<T>>;

    void read(); // one async_read_some into rx_
    void handle_frames(); // decodes every complete frame in rx_
    void handle_frame(proto::Message&& msg);
    void deliver(proto::Message&& m); // a received message: Ping answered here, the rest to on_message
    void written(); // after appending to pending_: the buffer cap, then the flush
    bool lagged() const { return limits_.simLatency.count() > 0 || limits_.simJitter.count() > 0; }
    template <class T> void delay(DelayQueue<T>& queue, asio::steady_timer& timer, T item);
    template <class T> void arm_delay(DelayQueue<T>& queue, asio::steady_timer& timer);
    void schedule_flush();
    void flush();
    void arm_heartbeat();
    void close();

    uint32_t id_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    SessionLimits limits_;
    OnMessage onMessage_;
    OnClosed onClosed_;
    asio::ssl::stream_base::handshake_type role_;
    std::string remote_;

    std::vector<uint8_t> rx_; // received, not yet decoded: at most one partial frame between reads
    std::vector<uint8_t> pending_, writing_;
    bool flushScheduled_ = false;
    bool writing_now_ = false;
    bool closing_ = false;
    bool closed_ = false;
    bool tlsDone_ = false;

    asio::steady_timer handshakeTimer_;
    asio::steady_timer heartbeatTimer_;
    asio::steady_timer closeTimer_;
    DelayQueue<proto::Message> lagIn_;
    DelayQueue<std::vector<uint8_t>> lagOut_;
    asio::steady_timer lagInTimer_, lagOutTimer_;
    std::minstd_rand lagRng_;
    TimePoint lastReceive_;
    uint32_t pingNonce_ = 0;
    MessageRateLimiter rate_;
};

// Encodes m once and appends that frame to every session in to (send_frame); no encode when to is empty.
void send_many(std::span<Session* const> to, const proto::Message& m);

} // namespace kast::net
