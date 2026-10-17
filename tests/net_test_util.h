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

#include <codec.h>

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
// a "/boom/..." target answers 500 with a non-JSON body. /v1/realms lists Ashenvale (1, 127.0.0.1:4000, online), Duskmoor
// (2, 127.0.0.1:4001, maintenance) and Testgrove (3, 127.0.0.1:worldPort, online: the FakeWorld). Any path prefix before /v1/ is ignored; "/stall/..." waits `stall`
// first (to catch requests in flight for screenshots). Tokens are unsigned JWTs with sub "42".
// Also GET /health (200 {status, db}, or 503 db_unavailable while dbDown) and the admin endpoints: kAdminEmail /
// kDemoPassword logs in as account 1 with role admin; /v1/admin/* need its latest token (else 401 unauthorized, or 403
// forbidden for a player token). Realm create / update / delete edit the list /v1/realms answers (409
// realm_name_taken, 404 realm_not_found, 400 validation_error without name / host / port / region); ban, unban and
// role answer 400 cannot_modify_self for account 1, 404 account_not_found for ids from 1000, and ban needs a reason.
constexpr const char* kDemoEmail = "demo@example.com";
constexpr const char* kAdminEmail = "admin@example.com";
constexpr const char* kDemoPassword = "demo-password-123";

class FakeAuth {
public:
    std::atomic<int> expiresIn = 900;       // seconds, in each new session
    std::atomic<bool> refreshFails = false; // refresh answers 401 invalid_refresh_token
    std::atomic<int> refreshDelayMs = 0;
    std::chrono::milliseconds stall{3000};
    std::atomic<int> worldPort = 47707; // Testgrove's port
    std::atomic<bool> dbDown = false;   // /health answers 503

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
    // An /v1/admin/... request, with mutex_ held and the caller checked as admin.
    http::response<http::string_body> admin(const std::string& method, const std::string& path, const nlohmann::json& body);
    nlohmann::json& realms(); // the realm list, made on first use

    mutable std::mutex mutex_;
    std::vector<Seen> seen_;
    std::string accessToken_;
    std::string sessionEmail_ = kDemoEmail; // whose session the latest token is
    nlohmann::json realms_;
    int nextRealmId_ = 4;
    int issued_ = 0, refreshes_ = 0;
};

// A Server body answering fake's requests, one connection after another.
Server::Body serveFakeAuth(FakeAuth& fake);

// A fake world server (docs/services/world/protocol.html) for kast.world tests, scripted by what the client sends. Hello must
// be protocol 2 (proto::kProtocolVersion); Auth gets Ping{kFakeWorldPing} then AuthOk{42, player} (an empty token: Disconnect{invalid_token}).
// Each connection has its own characters. CharListReq -> CharList. CharCreate: "Taken" -> CharError{name_taken};
// "Kick<n>" -> Disconnect{code n} and close; "Garbage" -> a malformed Spawn frame; "Unknown" -> opcode 0x7777;
// "Split" -> CharCreated written in two parts 50 ms apart; anything else -> CharCreated{id from 100, name,
// (100, 0, 100)}. CharDelete / EnterWorld of an unknown id -> CharError{not_found}. EnterWorld -> EnterWorldOk{1,
// (100, 0, 100), 0.5}, Spawn{2, "Other", (101, 0, 100), 0}, EntityMove{2, (102, 0, 100), 1}, Correction{(100, 0, 100)}
// in one write. Move more than 20 m (x/z) from the last accepted position (at first the spawn) -> Correction{that
// position}, else it is accepted silently. Say -> ChatMsg{the character's name, text}, then Despawn{2} if text is "bye", or
// ComponentUpdate{2, Health, {current 42.5, max 100}}, a ComponentUpdate with invalid JSON and ComponentRemove{2, Health}
// if text is "components", or ServerEvent{quest.update, {quest 7, steps [1, 2], done false}}, a ServerEvent with invalid
// JSON and ServerEvent{ping, null} if text is "events", or the NPC Spawn{3, "Guard", (103, 0, 100), 0.25, npc, "Guard"} if
// text is "npc". Anything else:
// Disconnect{unexpected_message}.
constexpr uint32_t kFakeWorldPing = 4242;

class FakeWorld {
public:
    void record(proto::Message message);
    std::vector<proto::Message> seen() const; // every client message so far, all connections
    int pongs() const;                        // Pong{kFakeWorldPing} received

private:
    mutable std::mutex mutex_;
    std::vector<proto::Message> seen_;
};

// A TLS Server body (construct the Server with a cert) running fake's connections one after another.
Server::Body serveFakeWorld(FakeWorld& fake);

} // namespace kast::nettest
