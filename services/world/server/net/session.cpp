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
        self->read_header();
        self->arm_heartbeat();
        self->flush(); // what a client-role session sent before the handshake finished
    });
}

void Session::read_header() {
    auto self = shared_from_this();
    asio::async_read(stream_, asio::buffer(header_), [self](asio::error_code ec, size_t) {
        if (self->closed_ || self->closing_) return;
        if (ec) { self->close(); return; }
        const proto::Header h = proto::parse_header(self->header_);
        if (h.length > self->limits_.maxFrameBytes) { self->disconnect(proto::Code::frame_too_large); return; }
        self->read_payload(h.length, h.opcode);
    });
}

void Session::read_payload(uint16_t length, proto::Opcode op) {
    payload_.resize(length);
    if (length == 0) { handle_frame(op); return; }
    auto self = shared_from_this();
    asio::async_read(stream_, asio::buffer(payload_), [self, op](asio::error_code ec, size_t) {
        if (self->closed_ || self->closing_) return;
        if (ec) { self->close(); return; }
        self->handle_frame(op);
    });
}

void Session::handle_frame(proto::Opcode op) {
    const TimePoint now = SteadyClock::now();
    lastReceive_ = now;
    const auto verdict = rate_.check(now);
    if (verdict == MessageRateLimiter::Verdict::disconnect) { disconnect(proto::Code::rate_limited); return; }
    if (verdict == MessageRateLimiter::Verdict::accept) {
        proto::Message msg;
        switch (proto::decode_payload(op, payload_, msg)) {
        case proto::DecodeStatus::ok: break;
        case proto::DecodeStatus::unknown_opcode: disconnect(proto::Code::unknown_opcode); return;
        default: disconnect(proto::Code::malformed); return;
        }
        if (lagged()) delay(lagIn_, lagInTimer_, std::move(msg));
        else deliver(std::move(msg));
    }
    if (!closing_) read_header();
}

void Session::deliver(proto::Message&& m) {
    if (auto* ping = std::get_if<proto::Ping>(&m)) send(proto::Pong{ping->nonce});
    else if (!std::holds_alternative<proto::Pong>(m)) onMessage_(*this, std::move(m));
}

void Session::send(const proto::Message& m) {
    if (closing_) return;
    if (lagged()) delay(lagOut_, lagOutTimer_, m);
    else write(m);
}

// The lag simulator: messages wait in queue until due (lag_due), then go on (received ones to deliver, sent ones to
// write) in order. A Disconnect is not delayed; whatever is still queued when the session closes is dropped.
void Session::delay(DelayQueue& queue, asio::steady_timer& timer, proto::Message m) {
    const TimePoint now = SteadyClock::now();
    queue.push_back({lag_due(now, queue.empty() ? now : queue.back().due, limits_.simLatency, limits_.simJitter, lagRng_),
                     std::move(m)});
    if (queue.size() == 1) arm_delay(queue, timer);
}

void Session::arm_delay(DelayQueue& queue, asio::steady_timer& timer) {
    timer.expires_at(queue.front().due);
    timer.async_wait([self = shared_from_this(), &queue, &timer](asio::error_code ec) {
        if (ec || self->closing_) return;
        const bool inbound = &queue == &self->lagIn_;
        while (!queue.empty() && queue.front().due <= SteadyClock::now()) {
            proto::Message m = std::move(queue.front().msg);
            queue.pop_front();
            if (inbound) self->deliver(std::move(m));
            else self->write(m);
            if (self->closing_) return;
        }
        if (!queue.empty()) self->arm_delay(queue, timer);
    });
}

void Session::write(const proto::Message& m) {
    proto::encode(pending_, m);
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
