// The world server's admin port (WORLD_ADMIN_*; docs/services/world/protocol.html#admin): a TLS listener for the admin
// service. Each connection starts with LinkHello{WORLD_ADMIN_SECRET} (compared in constant time; wrong: logged,
// Disconnect{invalid_token}), then asks for the online list, kicks, or the live map contexts (instances). Knows nothing about the game: GameServer answers
// through the callbacks. All calls on the io thread.
#pragma once
#include "../net/session.h"

#include <messages.h>

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kast {

class AdminPort {
public:
    struct Options {
        std::string host;
        uint16_t port = 7780;
        std::string secret;
    };
    using Online = std::function<std::vector<proto::AdminOnlineEntry>()>;
    using Kick = std::function<uint32_t(const proto::AdminKick&)>; // sessions kicked
    using Instances = std::function<std::vector<proto::AdminInstanceEntry>()>;

    AdminPort(asio::io_context& io, asio::ssl::context& tls, const net::SessionLimits& limits, Options options,
              Online online, Kick kick, Instances instances);

    void start(); // throws std::system_error when the address can't be bound
    void stop();  // stops accepting and closes every admin connection

private:
    struct Conn {
        std::shared_ptr<net::Session> session;
        bool ok = false; // the secret matched
    };

    void accept();
    void on_message(net::Session& s, proto::Message&& m);

    asio::ssl::context& tls_;
    net::SessionLimits limits_;
    Options opts_;
    Online online_;
    Kick kick_;
    Instances instances_;
    asio::ip::tcp::acceptor acceptor_;
    std::unordered_map<uint32_t, Conn> conns_;
    uint32_t nextId_ = 1;
    bool stopped_ = false;
};

} // namespace kast
