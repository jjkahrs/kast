#include "session.h"
#include "../core/log.h"

namespace kast::net {

Session::Session(uint32_t id, asio::ip::tcp::socket socket, asio::ssl::context& tls, const SessionLimits& limits,
                 OnMessage onMessage, OnClosed onClosed, asio::ssl::stream_base::handshake_type role)
    : id_(id), stream_(std::move(socket), tls), limits_(limits), onMessage_(std::move(onMessage)),
      onClosed_(std::move(onClosed)), role_(role), handshakeTimer_(stream_.get_executor()), heartbeatTimer_(stream_.get_executor()),
      closeTimer_(stream_.get_executor()), lagInTimer_(stream_.get_executor()), lagOutTimer_(stream_.get_executor()),
      lagRng_(id), lastReceive_(SteadyClock::now()),
      rate_(limits.msgRate, limits.msgBurst, limits.msgRateGrace) {
    asio::error_code ec;
    const auto ep = stream_.lowest_layer().remote_endpoint(ec);
    if (!ec) remote_ = ep.address().to_string() + ":" + std::to_string(ep.port());
}

void Session::start(std::optional<proto::Code> rejectWith) {
    auto self = shared_from_this();
    // Covers TLS + Hello + Auth: the whole handshake must finish in time.
    handshakeTimer_.expires_after(limits_.handshakeTimeout);
    handshakeTimer_.async_wait([self](asio::error_code ec) {
        if (ec || self->closed_) return;
        log::info("handshake timeout", {{"session", self->id_}, {"remote", self->remote_}, {"tls", self->tlsDone_}});
        self->disconnect(proto::Code::handshake_timeout);
    });
    stream_.async_handshake(role_, [self, rejectWith](asio::error_code ec) {
        if (self->closed_) return;
        if (ec) {
            log::debug("tls handshake failed", {{"session", self->id_}, {"remote", self->remote_}, {"error", ec.message()}});
            self->close();
            return;
        }
        self->tlsDone_ = true;
        if (rejectWith) { self->disconnect(*rejectWith); return; }
        self->read();
        self->arm_heartbeat();
        self->flush(); // what a client-role session sent before the handshake finished
    });
}

// ponytail: 16 KB per read; one TLS record is at most 16 KB, so a bigger chunk would rarely fill.
constexpr size_t kReadChunk = 16 * 1024;

// Before each read rx_ holds at most one partial frame (< kHeaderSize + maxFrameBytes: a complete one is decoded, an
// oversized header disconnects), so it never exceeds kHeaderSize + maxFrameBytes + kReadChunk.
void Session::read() {
    const size_t kept = rx_.size();
    rx_.resize(kept + kReadChunk);
    ++readsIssued;
    auto self = shared_from_this();
    stream_.async_read_some(asio::buffer(rx_.data() + kept, kReadChunk), [self, kept](asio::error_code ec, size_t n) {
        if (self->closed_ || self->closing_) return;
        if (ec) { self->close(); return; }
        self->rx_.resize(kept + n);
        self->handle_frames();
    });
}

// Every complete frame in rx_, in order; the next read only once all of them are handled and nothing closed.
void Session::handle_frames() {
    size_t used = 0;
    while (!closing_ && !closed_) {
        proto::Decoded d = proto::decode_frame(std::span(rx_).subspan(used), limits_.maxFrameBytes);
        switch (d.status) {
        case proto::DecodeStatus::ok: break;
        case proto::DecodeStatus::need_more:
            rx_.erase(rx_.begin(), rx_.begin() + static_cast<std::ptrdiff_t>(used));
            read();
            return;
        case proto::DecodeStatus::too_large: disconnect(proto::Code::frame_too_large); return;
        case proto::DecodeStatus::unknown_opcode: disconnect(proto::Code::unknown_opcode); return;
        default: disconnect(proto::Code::malformed); return;
        }
        used += d.consumed;
        handle_frame(std::move(d.msg));
    }
}

void Session::handle_frame(proto::Message&& msg) {
    const TimePoint now = SteadyClock::now();
    lastReceive_ = now;
    // The frame is already decoded: a malformed one disconnects even when the limiter would have dropped it.
    const auto verdict = rate_.check(now);
    if (verdict == MessageRateLimiter::Verdict::disconnect) { disconnect(proto::Code::rate_limited); return; }
    if (verdict == MessageRateLimiter::Verdict::drop) return;
    if (lagged()) delay(lagIn_, lagInTimer_, std::move(msg));
    else deliver(std::move(msg));
}

void Session::deliver(proto::Message&& m) {
    if (auto* ping = std::get_if<proto::Ping>(&m)) send(proto::Pong{ping->nonce});
    else if (!std::holds_alternative<proto::Pong>(m)) onMessage_(*this, std::move(m));
}

void Session::send(const proto::Message& m) {
    if (closing_) return;
    ++framesEncoded;
    if (!lagged()) {
        proto::encode(pending_, m);
        written();
        return;
    }
    std::vector<uint8_t> frame;
    proto::encode(frame, m);
    delay(lagOut_, lagOutTimer_, std::move(frame));
}

void Session::send_frame(std::span<const uint8_t> frame) {
    if (closing_) return;
    if (lagged()) {
        // ponytail: a copy per lagged session; share one buffer if the lag simulator ever runs at load-test scale.
        delay(lagOut_, lagOutTimer_, std::vector<uint8_t>(frame.begin(), frame.end()));
        return;
    }
    pending_.insert(pending_.end(), frame.begin(), frame.end());
    written();
}

void send_many(std::span<Session* const> to, const proto::Message& m) {
    if (to.empty()) return;
    ++framesEncoded;
    std::vector<uint8_t> frame;
    proto::encode(frame, m);
    for (Session* s : to) s->send_frame(frame);
}

// The lag simulator: messages wait in queue until due (lag_due), then go on (received ones to deliver, sent frames to
// pending_) in order. A Disconnect is not delayed; whatever is still queued when the session closes is dropped.
template <class T> void Session::delay(DelayQueue<T>& queue, asio::steady_timer& timer, T item) {
    const TimePoint now = SteadyClock::now();
    queue.push_back({lag_due(now, queue.empty() ? now : queue.back().due, limits_.simLatency, limits_.simJitter, lagRng_),
                     std::move(item)});
    if (queue.size() == 1) arm_delay(queue, timer);
}

template <class T> void Session::arm_delay(DelayQueue<T>& queue, asio::steady_timer& timer) {
    timer.expires_at(queue.front().due);
    timer.async_wait([self = shared_from_this(), &queue, &timer](asio::error_code ec) {
        if (ec || self->closing_) return;
        while (!queue.empty() && queue.front().due <= SteadyClock::now()) {
            T item = std::move(queue.front().item);
            queue.pop_front();
            if constexpr (std::is_same_v<T, proto::Message>) {
                self->deliver(std::move(item));
            } else {
                self->pending_.insert(self->pending_.end(), item.begin(), item.end());
                self->written();
            }
            if (self->closing_) return;
        }
        if (!queue.empty()) self->arm_delay(queue, timer);
    });
}

void Session::written() {
    if (pending_.size() > limits_.maxSendBuffer) {
        log::warn("send buffer full, dropping slow client", {{"session", id_}, {"bytes", pending_.size()}});
        close();
        return;
    }
    schedule_flush();
}

// Everything sent during one handler run (e.g. a whole tick) goes out as one write.
void Session::schedule_flush() {
    if (flushScheduled_) return;
    flushScheduled_ = true;
    asio::post(stream_.get_executor(), [self = shared_from_this()] {
        self->flushScheduled_ = false;
        self->flush();
    });
}

void Session::flush() {
    if (closed_ || writing_now_ || pending_.empty() || !tlsDone_) return;
    writing_.swap(pending_);
    pending_.clear();
    writing_now_ = true;
    asio::async_write(stream_, asio::buffer(writing_), [self = shared_from_this()](asio::error_code ec, size_t) {
        self->writing_now_ = false;
        self->writing_.clear();
        if (self->closed_) return;
        if (ec) { self->close(); return; }
        if (!self->pending_.empty()) self->flush();
        else if (self->closing_) self->close();
    });
}

void Session::disconnect(proto::Code reason) {
    if (closing_) return;
    log::info("disconnect", {{"session", id_}, {"reason", proto::code_name(reason)}});
    if (!tlsDone_) { close(); return; } // no TLS channel to say why on
    proto::encode(pending_, proto::Disconnect{reason});
    closing_ = true;
    handshakeTimer_.cancel();
    heartbeatTimer_.cancel();
    closeTimer_.expires_after(Millis(2000)); // a client that never reads must not keep the socket open
    closeTimer_.async_wait([self = shared_from_this()](asio::error_code ec) {
        if (!ec) self->close();
    });
    flush();
}

void Session::authenticated() { handshakeTimer_.cancel(); }

void Session::arm_heartbeat() {
    heartbeatTimer_.expires_after(std::min(limits_.pingInterval, limits_.idleTimeout));
    heartbeatTimer_.async_wait([self = shared_from_this()](asio::error_code ec) {
        if (ec || self->closing_) return;
        if (SteadyClock::now() - self->lastReceive_ > self->limits_.idleTimeout) {
            self->disconnect(proto::Code::idle_timeout);
            return;
        }
        self->send(proto::Ping{++self->pingNonce_});
        self->arm_heartbeat();
    });
}

void Session::close() {
    if (closed_) return;
    closed_ = true;
    closing_ = true;
    handshakeTimer_.cancel();
    heartbeatTimer_.cancel();
    closeTimer_.cancel();
    lagInTimer_.cancel();
    lagOutTimer_.cancel();
    // ponytail: no TLS close_notify; peers see EOF. Add async_shutdown with a timeout if clients ever need it.
    asio::error_code ec;
    stream_.lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    stream_.lowest_layer().close(ec);
    if (onClosed_) onClosed_(*this);
}

} // namespace kast::net
