#include "net_test_util.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <format>
#include <stdexcept>

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
    const nlohmann::json claims{{"sub", "42"}, {"email", email}, {"role", "player"}, {"n", n}};
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
    if (path == "/v1/auth/login") {
        if (field("email") == "locked@test")
            return error(423, "account_locked", "Account locked", {{"lockedUntil", "2099-01-01T00:15:00.000Z"}});
        if (field("email") == "banned@test")
            return error(403, "account_banned", "Account banned", {{"reason", "cheating"}, {"expiresAt", nullptr}});
        if (field("email") != kDemoEmail || field("password") != kDemoPassword)
            return error(401, "invalid_credentials", "Invalid email or password");
        return reply(200, session(kDemoEmail));
    }
    if (path == "/v1/auth/refresh") {
        ++refreshes_;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(refreshDelayMs.load()));
        lock.lock();
        if (refreshFails)
            return error(401, "invalid_refresh_token", "Invalid refresh token");
        return reply(200, session(kDemoEmail));
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
        nlohmann::json realms = nlohmann::json::array();
        realms.push_back({{"id", 1}, {"name", "Ashenvale"}, {"host", "127.0.0.1"}, {"port", 4000},
                          {"region", "us-east"}, {"status", "online"}, {"sortOrder", 0}});
        realms.push_back({{"id", 2}, {"name", "Duskmoor"}, {"host", "127.0.0.1"}, {"port", 4001},
                          {"region", "eu-west"}, {"status", "maintenance"}, {"sortOrder", 1}});
        return reply(200, {{"realms", realms}});
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
