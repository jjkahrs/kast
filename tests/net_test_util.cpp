#include "net_test_util.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

namespace kast::nettest {
namespace {

std::string pem(BIO* bio)
{
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    std::string out(data, static_cast<size_t>(size));
    BIO_free(bio);
    return out;
}

void addExtension(X509* cert, int nid, const std::string& value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value.c_str());
    if (!ext)
        throw std::runtime_error("bad certificate extension " + value);
    X509_add_ext(cert, ext, -1);
    X509_EXTENSION_free(ext);
}

std::string base64Url(const std::string& bytes)
{
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    out.resize(static_cast<size_t>(n));
    while (!out.empty() && out.back() == '=')
        out.pop_back();
    for (char& c : out)
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    return out;
}

http::response<http::string_body> reply(unsigned status, const nlohmann::json& body)
{
    http::response<http::string_body> res{static_cast<http::status>(status), 11};
    if (!body.is_null()) {
        res.set(http::field::content_type, "application/json");
        res.body() = body.dump();
    }
    res.prepare_payload();
    return res;
}

http::response<http::string_body> error(unsigned status, const std::string& code, const std::string& message,
                                         const nlohmann::json& details = nullptr)
{
    nlohmann::json e{{"code", code}, {"message", message}};
    if (!details.is_null())
        e["details"] = details;
    return reply(status, {{"error", e}});
}

} // namespace

nlohmann::json FakeAuth::session(const std::string& email) // mutex_ held
{
    const int n = ++issued_;
    const bool admin = email == kAdminEmail;
    sessionEmail_ = email;
    const nlohmann::json claims{{"sub", admin ? "1" : "42"}, {"email", email}, {"role", admin ? "admin" : "player"}, {"n", n}};
    accessToken_ = base64Url(R"({"alg":"none","typ":"JWT"})") + "." + base64Url(claims.dump()) + ".sig";
    return {{"tokenType", "Bearer"},
            {"accessToken", accessToken_},
            {"expiresIn", expiresIn.load()},
            {"accessTokenExpiresAt", "2099-01-01T00:00:00.000Z"},
            {"refreshToken", std::format("refresh-{}", n)},
            {"refreshTokenExpiresAt", "2099-01-01T00:00:00.000Z"}};
}

http::response<http::string_body> FakeAuth::answer(const http::request<http::string_body>& req)
{
    const std::string target(req.target());
    if (target.starts_with("/stall/"))
        std::this_thread::sleep_for(stall);
    if (target.starts_with("/boom/")) {
        http::response<http::string_body> res{http::status::internal_server_error, 11};
        res.body() = "<html>boom</html>";
        res.prepare_payload();
        return res;
    }
    const size_t v1 = target.find("/v1/");
    const std::string path = v1 == std::string::npos ? target : target.substr(v1);
    const nlohmann::json body = nlohmann::json::parse(req.body(), nullptr, false);
    const auto field = [&](const char* key) {
        return body.is_object() && body.contains(key) && body[key].is_string() ? body[key].get<std::string>()
                                                                                 : std::string();
    };
    std::unique_lock lock(mutex_);
    seen_.push_back({std::string(req.method_string()), path, std::string(req[http::field::authorization]),
                     body.is_discarded() ? nlohmann::json() : body});
    const bool bearer = !accessToken_.empty() && req[http::field::authorization] == "Bearer " + accessToken_;
    const nlohmann::json checkEmail{{"message", "Check your email"}};
    if (target.ends_with("/health")) {
        if (dbDown)
            return error(503, "db_unavailable", "Database unavailable");
        return reply(200, {{"status", "ok"}, {"db", "ok"}});
    }
    if (path.starts_with("/v1/admin/")) {
        if (!bearer)
            return error(401, "unauthorized", "Missing or invalid access token");
        if (sessionEmail_ != kAdminEmail)
            return error(403, "forbidden", "Admin role required");
        return admin(std::string(req.method_string()), path, body);
    }
    if (path == "/v1/auth/login") {
        if (field("email") == "locked@test")
            return error(423, "account_locked", "Account locked", {{"lockedUntil", "2099-01-01T00:15:00.000Z"}});
        if (field("email") == "banned@test")
            return error(403, "account_banned", "Account banned", {{"reason", "cheating"}, {"expiresAt", nullptr}});
        if ((field("email") != kDemoEmail && field("email") != kAdminEmail) || field("password") != kDemoPassword)
            return error(401, "invalid_credentials", "Invalid email or password");
        return reply(200, session(field("email")));
    }
    if (path == "/v1/auth/refresh") {
        ++refreshes_;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(refreshDelayMs.load()));
        lock.lock();
        if (refreshFails)
            return error(401, "invalid_refresh_token", "Invalid refresh token");
        return reply(200, session(sessionEmail_));
    }
    if (path == "/v1/auth/register") {
        if (field("password").size() < 10)
            return error(400, "validation_error", "Request validation failed",
                         nlohmann::json::array({{{"path", "password"}, {"message", "Too short"}}}));
        return reply(202, checkEmail);
    }
    if (path == "/v1/auth/resend-verification" || path == "/v1/auth/forgot-password")
        return reply(202, checkEmail);
    if (path == "/v1/auth/verify-email" || path == "/v1/auth/reset-password") {
        if (field("token") == "bad")
            return error(400, "invalid_token", "Invalid or expired token");
        return reply(200, {{"message", "Done"}});
    }
    if (path == "/v1/auth/logout")
        return reply(204, nullptr);
    if (path == "/v1/me" || path == "/v1/realms") {
        if (!bearer)
            return error(401, "unauthorized", "Missing or invalid access token");
        if (path == "/v1/me")
            return reply(200, {{"id", 42}, {"email", kDemoEmail}, {"role", "player"}, {"emailVerified", true},
                               {"createdAt", "2026-01-01T00:00:00.000Z"}});
        return reply(200, {{"realms", realms()}});
    }
    return error(404, "not_found", "Not found");
}

nlohmann::json& FakeAuth::realms() // mutex_ held
{
    if (realms_.is_null()) {
        realms_ = nlohmann::json::array();
        realms_.push_back({{"id", 1}, {"name", "Ashenvale"}, {"host", "127.0.0.1"}, {"port", 4000},
                           {"region", "us-east"}, {"status", "online"}, {"sortOrder", 0}});
        realms_.push_back({{"id", 2}, {"name", "Duskmoor"}, {"host", "127.0.0.1"}, {"port", 4001},
                           {"region", "eu-west"}, {"status", "maintenance"}, {"sortOrder", 1}});
        realms_.push_back({{"id", 3}, {"name", "Testgrove"}, {"host", "127.0.0.1"}, {"port", worldPort.load()},
                           {"region", "us-west"}, {"status", "online"}, {"sortOrder", 2}});
    }
    return realms_;
}

http::response<http::string_body> FakeAuth::admin(const std::string& method, const std::string& path,
                                                  const nlohmann::json& body) // mutex_ held
{
    nlohmann::json& list = realms();
    const auto realmWith = [&](const char* key, const nlohmann::json& value) {
        return std::ranges::find_if(list, [&](const nlohmann::json& r) { return r[key] == value; });
    };
    // "/v1/admin/<kind>/<id>[/<action>]"
    const std::string rest = path.substr(std::string_view("/v1/admin/").size());
    const size_t slash = rest.find('/');
    const std::string kind = rest.substr(0, slash);
    std::string idText = slash == std::string::npos ? "" : rest.substr(slash + 1);
    const std::string action = idText.find('/') == std::string::npos ? "" : idText.substr(idText.find('/') + 1);
    idText = idText.substr(0, idText.find('/'));
    const long long id = idText.empty() ? 0 : std::atoll(idText.c_str());
    if (kind == "realms") {
        const bool valid = body.is_object() && body.contains("name") && body["name"].is_string() &&
                           body.contains("host") && body["host"].is_string() && body.contains("port") &&
                           body["port"].is_number_integer() && body.contains("region") && body["region"].is_string();
        if (idText.empty() && method == "GET")
            return reply(200, {{"realms", list}});
        if ((method == "POST" || method == "PUT") && !valid)
            return error(400, "validation_error", "Request validation failed");
        const auto it = realmWith("id", id);
        if (!idText.empty() && it == list.end())
            return error(404, "realm_not_found", "Realm not found");
        if (method == "GET")
            return reply(200, *it);
        if (method == "DELETE") {
            list.erase(it);
            return reply(204, nullptr);
        }
        if (const auto named = realmWith("name", body["name"]); named != list.end() && named != it)
            return error(409, "realm_name_taken", "A realm with that name exists");
        nlohmann::json realm{{"id", idText.empty() ? nextRealmId_++ : id}, {"name", body["name"]}, {"host", body["host"]},
                             {"port", body["port"]}, {"region", body["region"]},
                             {"status", body.value("status", "offline")}, {"sortOrder", body.value("sortOrder", 0)}};
        if (method == "POST") {
            list.push_back(realm);
            return reply(201, realm);
        }
        *it = realm;
        return reply(200, realm);
    }
    if (kind == "accounts" && (action == "ban" || action == "role")) {
        if (id == 1)
            return error(400, "cannot_modify_self", "You cannot do that to your own account");
        if (id <= 0 || id >= 1000)
            return error(404, "account_not_found", "Account not found");
        if (action == "role")
            return reply(200, {{"id", id}, {"email", std::format("user{}@example.com", id)}, {"role", body.value("role", "")},
                               {"emailVerified", true}, {"createdAt", "2026-01-01T00:00:00.000Z"}});
        if (method == "DELETE")
            return reply(204, nullptr);
        if (!body.is_object() || body.value("reason", "").empty())
            return error(400, "validation_error", "Request validation failed");
        return reply(201, {{"accountId", id}, {"reason", body["reason"]}, {"expiresAt", body.value("expiresAt", nlohmann::json())},
                           {"createdAt", "2026-01-01T00:00:00.000Z"}});
    }
    return error(404, "not_found", "Not found");
}

std::vector<FakeAuth::Seen> FakeAuth::seen() const
{
    const std::lock_guard lock(mutex_);
    return seen_;
}

std::string FakeAuth::accessToken() const
{
    const std::lock_guard lock(mutex_);
    return accessToken_;
}

int FakeAuth::refreshes() const
{
    const std::lock_guard lock(mutex_);
    return refreshes_;
}

Server::Body serveFakeAuth(FakeAuth& fake)
{
    return [&fake](Server& server) {
        while (!server.stopping()) {
            tcp::socket socket = server.accept();
            try {
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                http::read(socket, buffer, req);
                http::write(socket, fake.answer(req));
            } catch (const std::exception&) { // the client went away, or the destructor's wake-up connect
            }
        }
    };
}

void FakeWorld::record(proto::Message message)
{
    const std::lock_guard lock(mutex_);
    seen_.push_back(std::move(message));
}

std::vector<proto::Message> FakeWorld::seen() const
{
    const std::lock_guard lock(mutex_);
    return seen_;
}

int FakeWorld::pongs() const
{
    const std::lock_guard lock(mutex_);
    return static_cast<int>(std::ranges::count_if(seen_, [](const proto::Message& m) {
        const auto* pong = std::get_if<proto::Pong>(&m);
        return pong && pong->nonce == kFakeWorldPing;
    }));
}

namespace {

// One world connection until either side closes it.
void runFakeWorld(FakeWorld& fake, TlsSocket& socket)
{
    std::vector<uint8_t> out; // frames of the current answer, written at once
    const auto queue = [&](const proto::Message& m) { proto::encode(out, m); };
    const auto raw = [&](uint16_t opcode, std::vector<uint8_t> payload) {
        const auto n = static_cast<uint16_t>(payload.size());
        out.insert(out.end(), {static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(opcode),
                               static_cast<uint8_t>(opcode >> 8)});
        out.insert(out.end(), payload.begin(), payload.end());
    };
    const auto flush = [&] {
        asio::write(socket, asio::buffer(out));
        out.clear();
    };
    std::vector<proto::CharInfo> chars;
    uint64_t nextId = 100;
    std::string playing;
    const proto::Vec3 spawn{100, 0, 100};
    proto::Vec3 valid = spawn; // the last accepted Move
    uint32_t epoch = 0;        // bumped by each rejection; older Moves are dropped silently, like the real server
    bool holdNext = false;     // Say "hold": the next Correction waits until 3 more Moves arrived (in flight behind it)
    int heldFor = 0;           // Moves still to come before the held Correction goes out
    bool rejectNext = false;   // Say "reject": the next Move is rejected whatever its distance (a small error)
    std::vector<uint8_t> held; // its frame
    for (;;) {
        uint8_t header[proto::kHeaderSize];
        asio::read(socket, asio::buffer(header));
        const proto::Header h = proto::parse_header(header);
        std::vector<uint8_t> payload(h.length);
        asio::read(socket, asio::buffer(payload));
        proto::Message message;
        if (proto::decode_payload(h.opcode, payload, message) != proto::DecodeStatus::ok) {
            queue(proto::Disconnect{proto::Code::malformed});
            return flush();
        }
        fake.record(message);
        const auto find = [&](uint64_t id) {
            return std::ranges::find_if(chars, [&](const proto::CharInfo& c) { return c.id == id; });
        };
        if (const auto* hello = std::get_if<proto::Hello>(&message)) {
            if (hello->protocolVersion != proto::kProtocolVersion) {
                queue(proto::Disconnect{proto::Code::protocol_mismatch});
                return flush();
            }
        } else if (const auto* auth = std::get_if<proto::Auth>(&message)) {
            if (auth->accessToken.empty()) {
                queue(proto::Disconnect{proto::Code::invalid_token});
                return flush();
            }
            queue(proto::Ping{kFakeWorldPing});
            queue(proto::AuthOk{42, proto::Role::player});
        } else if (std::holds_alternative<proto::Pong>(message)) {
        } else if (std::holds_alternative<proto::CharListReq>(message)) {
            queue(proto::CharList{chars});
        } else if (const auto* create = std::get_if<proto::CharCreate>(&message)) {
            const std::string& name = create->name;
            if (name == "Taken") {
                queue(proto::CharError{proto::Opcode::CharCreate, proto::Code::name_taken});
            } else if (name.starts_with("Kick")) {
                queue(proto::Disconnect{static_cast<proto::Code>(std::stoi(name.substr(4)))});
                return flush();
            } else if (name == "Garbage") {
                raw(static_cast<uint16_t>(proto::Opcode::Spawn), {1, 2, 3}); // a Spawn needs 4 + 2 + 12 + 4 bytes
            } else if (name == "Unknown") {
                raw(0x7777, {});
            } else {
                chars.push_back({nextId++, name, spawn, create->options});
                queue(proto::CharCreated{chars.back()});
                if (name == "Split") {
                    asio::write(socket, asio::buffer(out.data(), 3));
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    out.erase(out.begin(), out.begin() + 3);
                }
            }
        } else if (const auto* del = std::get_if<proto::CharDelete>(&message)) {
            if (const auto it = find(del->charId); it != chars.end()) {
                chars.erase(it);
                queue(proto::CharDeleted{del->charId});
            } else {
                queue(proto::CharError{proto::Opcode::CharDelete, proto::Code::not_found});
            }
        } else if (const auto* enter = std::get_if<proto::EnterWorld>(&message)) {
            if (const auto it = find(enter->charId); it != chars.end()) {
                playing = it->name;
                valid = spawn;
                epoch = 0;
                queue(proto::EnterWorldOk{1, 1, spawn, 0.5f});
                queue(proto::Spawn{2, "Other", {101, 0, 100}, 0});
                queue(proto::EntityMove{2, {102, 0, 100}, 1});
                queue(proto::Correction{spawn});
            } else {
                queue(proto::CharError{proto::Opcode::EnterWorld, proto::Code::not_found});
            }
        } else if (const auto* move = std::get_if<proto::Move>(&message)) {
            // Like the real server's speed check, coarsely: a jump over 20 m on x/z is rejected.
            if (move->epoch < epoch) {
                if (heldFor > 0 && --heldFor == 0)
                    out.insert(out.end(), held.begin(), held.end());
            } else if (std::exchange(rejectNext, false) || std::hypot(move->pos.x - valid.x, move->pos.z - valid.z) > 20.0f) {
                queue(proto::Correction{valid, move->seq, ++epoch});
                if (std::exchange(holdNext, false)) {
                    held.swap(out);
                    heldFor = 3;
                }
            } else {
                valid = move->pos;
            }
        } else if (const auto* say = std::get_if<proto::Say>(&message)) {
            queue(proto::ChatMsg{playing, say->text});
            if (say->text == "bye")
                queue(proto::Despawn{2});
            if (say->text == "components") {
                queue(proto::ComponentUpdate{2, "Health", R"({"current":42.5,"max":100})"});
                queue(proto::ComponentUpdate{2, "Health", "{not json"}); // ignored with a warning
                queue(proto::ComponentRemove{2, "Health"});
            }
            if (say->text == "events") {
                queue(proto::ServerEvent{"quest.update", R"({"quest":7,"steps":[1,2],"done":false})"});
                queue(proto::ServerEvent{"broken", "{not json"}); // ignored with a warning
                queue(proto::ServerEvent{"ping", "null"});
            }
            if (say->text == "hold")
                holdNext = true;
            if (say->text == "reject")
                rejectNext = true;
            if (say->text == "npc")
                queue(proto::Spawn{3, "Guard", {103, 0, 100}, 0.25f, proto::SpawnKind::npc, "Guard"});
            if (say->text == "mapchange") { // like a transfer: MapChange (a new epoch), then the old neighbour's Despawn
                valid = {300, 0, 310};
                queue(proto::MapChange{2, valid, 1.5f, ++epoch});
                queue(proto::Despawn{3});
            }
        } else if (const auto* event = std::get_if<proto::ClientEvent>(&message)) {
            if (event->requestId == 0) {
                queue(proto::ServerEvent{"echo." + event->name, event->json}); // kast.world.send's frame, back as an event
            } else if (event->name == "test.ok") { // answered twice: the second is ignored
                queue(proto::Reply{event->requestId, true, event->json});
                queue(proto::Reply{event->requestId, true, event->json});
            } else if (event->name == "test.no") {
                queue(proto::Reply{event->requestId, false, R"("no_way")"});
            } // any other request: never answered
        } else {
            queue(proto::Disconnect{proto::Code::unexpected_message});
            return flush();
        }
        flush();
    }
}

} // namespace

Server::Body serveFakeWorld(FakeWorld& fake)
{
    return [&fake](Server& server) {
        while (!server.stopping()) {
            try {
                TlsSocket socket = server.acceptTls();
                runFakeWorld(fake, socket);
                boost::system::error_code ec;
                socket.lowest_layer().shutdown(tcp::socket::shutdown_both, ec);
            } catch (const std::exception&) { // the client went away, or the destructor's wake-up connect
            }
        }
    };
}

Cert makeCert(const std::string& subjectAltName, int days)
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600);
    X509_time_adj_ex(X509_getm_notAfter(cert), days, 0, nullptr); // days, not seconds: a long is 32 bits here
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Kast test"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    addExtension(cert, NID_basic_constraints, "critical,CA:TRUE");
    addExtension(cert, NID_subject_alt_name, subjectAltName);
    X509_sign(cert, key, EVP_sha256());

    BIO* certBio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certBio, cert);
    BIO* keyBio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr);
    X509_free(cert);
    EVP_PKEY_free(key);
    return {pem(certBio), pem(keyBio)};
}

Server::Server(Body body, uint16_t port, const Cert* cert) : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), port})
{
    port_ = acceptor_.local_endpoint().port();
    if (cert) {
        tls_ = std::make_unique<ssl::context>(ssl::context::tls_server);
        tls_->use_certificate_chain(asio::buffer(cert->certPem));
        tls_->use_private_key(asio::buffer(cert->keyPem), ssl::context::pem);
    }
    thread_ = std::thread([this, body = std::move(body)] {
        try {
            body(*this);
        } catch (const std::exception& e) {
            const std::lock_guard lock(mutex_);
            error_ = e.what();
        }
    });
}

Server::~Server()
{
    stopping_ = true;
    for (int i = 0; i < 4; ++i) { // a few, in case the body accepts more than once before it notices
        boost::system::error_code ec;
        tcp::socket poke(io_);
        poke.connect({asio::ip::make_address("127.0.0.1"), port_}, ec);
    }
    thread_.join();
}

std::string Server::error() const
{
    const std::lock_guard lock(mutex_);
    return error_;
}

tcp::socket Server::accept()
{
    tcp::socket socket = acceptor_.accept();
    if (stopping_)
        throw std::runtime_error("stopping");
    ++accepted_;
    return socket;
}

TlsSocket Server::acceptTls()
{
    if (!tls_)
        throw std::logic_error("acceptTls needs a cert");
    TlsSocket stream(accept(), *tls_);
    stream.handshake(ssl::stream_base::server);
    return stream;
}

} // namespace kast::nettest
