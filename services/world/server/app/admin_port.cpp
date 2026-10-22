#include "admin_port.h"
#include "../core/log.h"

#include <openssl/crypto.h>

#include <algorithm>

namespace kast {
namespace {

constexpr size_t kMaxConnections = 32; // ponytail: the admin service opens one per request; raise if it pools

bool same_secret(const std::string& a, const std::string& b) {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

AdminPort::AdminPort(asio::io_context& io, asio::ssl::context& tls, const net::SessionLimits& limits, Options options,
                     Online online, Kick kick)
    : tls_(tls), limits_(limits), opts_(std::move(options)), online_(std::move(online)), kick_(std::move(kick)),
      acceptor_(io) {}

void AdminPort::start() {
    const asio::ip::tcp::endpoint ep(asio::ip::make_address(opts_.host), opts_.port);
    acceptor_.open(ep.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(ep);
    acceptor_.listen();
    log::info("admin port listening", {{"host", opts_.host}, {"port", opts_.port}});
    accept();
}

void AdminPort::stop() {
    stopped_ = true;
    asio::error_code ec;
    acceptor_.close(ec);
    std::vector<std::shared_ptr<net::Session>> sessions;
    for (auto& [id, c] : conns_) sessions.push_back(c.session);
    for (auto& s : sessions) s->disconnect(proto::Code::server_shutdown);
}

void AdminPort::accept() {
    acceptor_.async_accept([this](asio::error_code ec, asio::ip::tcp::socket socket) {
        if (stopped_ || !acceptor_.is_open()) return;
        if (!ec) {
            socket.set_option(asio::ip::tcp::no_delay(true), ec);
            const uint32_t id = nextId_++;
            auto session = std::make_shared<net::Session>(
                id, std::move(socket), tls_, limits_,
                [this](net::Session& s, proto::Message&& m) { on_message(s, std::move(m)); },
                [this](net::Session& s) { conns_.erase(s.id()); });
            if (conns_.size() >= kMaxConnections) {
                session->start(proto::Code::server_full);
            } else {
                conns_[id].session = session;
                session->start();
            }
        }
        accept();
    });
}

void AdminPort::on_message(net::Session& s, proto::Message&& m) {
    auto it = conns_.find(s.id());
    if (it == conns_.end() || stopped_) return;
    Conn& c = it->second;
    if (!c.ok) {
        const auto* hello = std::get_if<proto::LinkHello>(&m);
        if (!hello) return s.disconnect(proto::Code::unexpected_message);
        if (hello->protocolVersion != proto::kProtocolVersion) return s.disconnect(proto::Code::protocol_mismatch);
        if (!same_secret(hello->secret, opts_.secret)) { // never log the secret
            log::warn("admin port refused: wrong secret", {{"session", s.id()}, {"remote", s.remote()}});
            return s.disconnect(proto::Code::invalid_token);
        }
        c.ok = true;
        s.authenticated();
        s.send(proto::LinkOk{});
        log::info("admin connected", {{"session", s.id()}, {"remote", s.remote()}});
        return;
    }
    if (std::holds_alternative<proto::AdminOnlineReq>(m)) {
        const auto all = online_();
        proto::AdminOnline chunk;
        for (size_t i = 0; i < all.size(); i += proto::AdminOnline::kMaxEntries) {
            const size_t end = std::min(all.size(), i + proto::AdminOnline::kMaxEntries);
            chunk.entries.assign(all.begin() + i, all.begin() + end);
            chunk.more = end < all.size();
            s.send(chunk);
        }
        if (all.empty()) s.send(chunk);
    } else if (auto* kick = std::get_if<proto::AdminKick>(&m)) {
        const uint32_t n = kick_(*kick);
        log::info("admin kick", {{"session", s.id()}, {"remote", s.remote()}, {"account", kick->accountId},
                                 {"char", kick->charId}, {"reason", kick->reason}, {"kicked", n}});
        s.send(proto::AdminKicked{n});
    } else {
        s.disconnect(proto::Code::unexpected_message);
    }
}

} // namespace kast
