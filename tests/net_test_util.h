#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Loopback test servers for kast::Network: blocking Boost.Asio / Beast code on its own thread per server, plus
// self-signed certificates made at run time. Shared by kast_self_check (tests/net_self_check.cpp) and
// kast_net_test_server (tests/net_test_server.cpp).
namespace kast::nettest {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

struct Cert {
    std::string certPem, keyPem;
};
// A self-signed RSA certificate (its own CA) for subjectAltName, e.g. "DNS:localhost,IP:127.0.0.1", valid for days.
Cert makeCert(const std::string& subjectAltName, int days = 2);

using TlsSocket = ssl::stream<tcp::socket>;

// A 127.0.0.1 listener whose body runs on its own thread. The destructor unblocks a pending accept (by connecting to
// it) and joins, so destroy the clients first: a body blocked reading an open client socket would never return.
class Server {
public:
    using Body = std::function<void(Server&)>;
    // port 0: an ephemeral one. cert: for acceptTls (else it throws).
    explicit Server(Body body, uint16_t port = 0, const Cert* cert = nullptr);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    uint16_t port() const { return port_; }
    int accepted() const { return accepted_; } // connections accepted so far
    std::string error() const;                  // what the body threw ("" if nothing); EOF from a client counts

    // For the body. Blocking; throw on failure.
    tcp::socket accept();
    TlsSocket acceptTls(); // handshake done
    asio::io_context& io() { return io_; }
    ssl::context& tls() { return *tls_; }
    bool stopping() const { return stopping_; }

private:
    asio::io_context io_;
    std::unique_ptr<ssl::context> tls_;
    tcp::acceptor acceptor_;
    uint16_t port_ = 0;
    std::atomic<int> accepted_ = 0;
    std::atomic<bool> stopping_ = false;
    mutable std::mutex mutex_;
    std::string error_;
    std::thread thread_;
};

// Reads one '\n'-terminated line (without it) from stream, keeping the rest in buffer.
template <class Stream>
std::string readLine(Stream& stream, std::string& buffer)
{
    const size_t n = asio::read_until(stream, asio::dynamic_buffer(buffer), '\n');
    std::string line = buffer.substr(0, n - 1);
    buffer.erase(0, n);
    return line;
}

// A fake auth service (services/auth/openapi.yaml, player endpoints) for kast.auth tests, one request at a time.
// Stateless per input: login accepts kDemoEmail / kDemoPassword (401 invalid_credentials otherwise), "locked@test"
// gets 423 account_locked and "banned@test" 403 account_banned (both with details); register rejects passwords under
// 10 characters (400 validation_error with details); verify-email and reset-password reject the token "bad"
// (400 invalid_token); /v1/me and /v1/realms need "Authorization: Bearer <the latest access token>" (401 unauthorized);
// a "/boom/..." target answers 500 with a non-JSON body. Any path prefix before /v1/ is ignored; "/stall/..." waits `stall`
// first (to catch requests in flight for screenshots). Tokens are unsigned JWTs with sub "42".
constexpr const char* kDemoEmail = "demo@example.com";
constexpr const char* kDemoPassword = "demo-password-123";

class FakeAuth {
public:
    std::atomic<int> expiresIn = 900;       // seconds, in each new session
    std::atomic<bool> refreshFails = false; // refresh answers 401 invalid_refresh_token
    std::atomic<int> refreshDelayMs = 0;
    std::chrono::milliseconds stall{3000};

    http::response<http::string_body> answer(const http::request<http::string_body>& req);

    struct Seen {
        std::string method, path, authorization;
        nlohmann::json body;
    };
    std::vector<Seen> seen() const;  // every request so far
    std::string accessToken() const; // the latest issued
    int refreshes() const;

private:
    nlohmann::json session(const std::string& email);

    mutable std::mutex mutex_;
    std::vector<Seen> seen_;
    std::string accessToken_;
    int issued_ = 0, refreshes_ = 0;
};

// A Server body answering fake's requests, one connection after another.
Server::Body serveFakeAuth(FakeAuth& fake);

} // namespace kast::nettest
