// One TLS client connection: framing, frame/rate limits, handshake deadline, heartbeat and batched writes.
// Knows nothing about game rules; decoded messages go to on_message. All calls happen on the io thread.
#pragma once
#include "../core/clock.h"
#include "../core/rate_limiter.h"

#include <codec.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <optional>
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
};

class Session : public std::enable_shared_from_this<Session> {
public:
    using OnMessage = std::function<void(Session&, proto::Message&&)>;
    using OnClosed = std::function<void(Session&)>;

    Session(uint32_t id, asio::ip::tcp::socket socket, asio::ssl::context& tls, const SessionLimits& limits,
            OnMessage onMessage, OnClosed onClosed);

    // rejectWith: complete TLS, send Disconnect{rejectWith}, close (e.g. server_full).
    void start(std::optional<proto::Code> rejectWith = std::nullopt);
    void send(const proto::Message& m);
    // Sends Disconnect{reason}, stops reading, closes once it is written (or after a short grace).
    void disconnect(proto::Code reason);
    // Handshake finished (AuthOk sent): the handshake deadline no longer applies.
    void authenticated();

    uint32_t id() const { return id_; }
    bool closing() const { return closing_; }
    std::string remote() const { return remote_; }

private:
    void read_header();
    void read_payload(uint16_t length, proto::Opcode op);
    void handle_frame(proto::Opcode op);
    void schedule_flush();
    void flush();
    void arm_heartbeat();
    void close();

    uint32_t id_;
    asio::ssl::stream<asio::ip::tcp::socket> stream_;
    SessionLimits limits_;
    OnMessage onMessage_;
    OnClosed onClosed_;
    std::string remote_;

    std::array<uint8_t, proto::kHeaderSize> header_{};
    std::vector<uint8_t> payload_;
    std::vector<uint8_t> pending_, writing_;
    bool flushScheduled_ = false;
    bool writing_now_ = false;
    bool closing_ = false;
    bool closed_ = false;
    bool tlsDone_ = false;

    asio::steady_timer handshakeTimer_;
    asio::steady_timer heartbeatTimer_;
    asio::steady_timer closeTimer_;
    TimePoint lastReceive_;
    uint32_t pingNonce_ = 0;
    MessageRateLimiter rate_;
};

} // namespace kast::net
