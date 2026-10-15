#include "network.h"

#include "log.h"

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
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <charconv>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace kast {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using boost::system::error_code;
using Json = nlohmann::json;

using TcpStream = beast::tcp_stream;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

// Event text from the network (error messages, close reasons): non-ASCII bytes that aren't UTF-8 become '?', so the
// payload always serializes.
std::string printable(std::string text)
{
    if (!isUtf8(text))
        for (char& c : text)
            if (static_cast<unsigned char>(c) >= 0x80)
                c = '?';
    return text;
}

bool isIpAddress(const std::string& host)
{
    error_code ec;
    (void)asio::ip::make_address(host, ec);
    return !ec;
}

// Every certificate in the Windows ROOT store, so publicly trusted servers verify as they do for Windows apps.
void addSystemRoots(SSL_CTX* ctx)
{
    const HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store)
        return;
    X509_STORE* x509 = SSL_CTX_get_cert_store(ctx);
    for (PCCERT_CONTEXT cert = CertEnumCertificatesInStore(store, nullptr); cert;
         cert = CertEnumCertificatesInStore(store, cert)) {
        const unsigned char* der = cert->pbCertEncoded;
        if (X509* x = d2i_X509(nullptr, &der, static_cast<long>(cert->cbCertEncoded))) {
            X509_STORE_add_cert(x509, x);
            X509_free(x);
        }
    }
    CertCloseStore(store, 0);
}

// Peer and hostname verification always on. Throws std::invalid_argument if caPem holds no certificate.
std::shared_ptr<ssl::context> makeTlsContext(const std::string& caPem)
{
    auto ctx = std::make_shared<ssl::context>(ssl::context::tls_client);
    ctx->set_verify_mode(ssl::verify_peer);
    addSystemRoots(ctx->native_handle());
    if (!caPem.empty()) {
        error_code ec;
        ctx->add_certificate_authority(asio::buffer(caPem), ec);
        if (ec)
            throw std::invalid_argument("caFile holds no valid PEM certificate");
    }
    return ctx;
}

std::string hostHeader(const NetUrl& url)
{
    const bool defaultPort = url.port == ((url.scheme == "https" || url.scheme == "wss") ? 443 : 80);
    return defaultPort ? url.host : std::format("{}:{}", url.host, url.port);
}

// Received bytes as a "data" / "body" value: nullopt + error ("<what> is not valid ...") if they don't fit the encoding.
std::optional<Json> decode(NetEncoding encoding, std::string bytes, std::string_view what, std::string& error)
{
    switch (encoding) {
    case NetEncoding::Text:
        if (!isUtf8(bytes)) {
            error = std::format("{} is not valid UTF-8 (encoding 'text')", what);
            return std::nullopt;
        }
        return Json(std::move(bytes));
    case NetEncoding::Base64:
        return Json(base64Encode(bytes));
    case NetEncoding::Json:
        try {
            return Json::parse(bytes);
        } catch (const Json::parse_error& e) { // e.what() quotes the input, which may not be UTF-8
            error = std::format("{} is not valid JSON (byte {})", what, e.byte);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace

// --- Shared state ---

// One io_context thread runs every socket. Connection and request objects are touched only on it (asio::post from the
// owner); the mutex guards what both threads see: the result queue and the status tables.
struct Network::Impl {
    struct Conn;
    struct Request;
    template <class Stream> struct Link;
    template <class Stream> struct HttpRequest;

    struct Entry {
        NetConnectionStatus status;
        NetEncoding encoding;
        NetFraming framing;
        uint64_t serial; // tells a replaced connection's late status updates apart
        std::shared_ptr<Conn> conn;
    };

    std::thread::id owner = std::this_thread::get_id();
    std::unique_ptr<asio::io_context> io;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
    std::thread thread;
    std::shared_ptr<ssl::context> systemTls; // built on first use: reading the ROOT store takes a moment
    uint64_t nextSerial = 1, nextRequest = 1;

    // io thread only: what stop() must abort, including connections whose name close() already forgot.
    std::set<std::shared_ptr<Conn>> liveConns;
    std::set<std::shared_ptr<Request>> liveRequests;

    mutable std::mutex mutex; // guards everything below
    std::vector<NetEvent> events;
    std::vector<std::string> internalErrors; // handler exceptions, logged by poll() (log is main-thread only)
    std::map<std::string, Entry> conns;
    std::map<uint64_t, NetRequestStatus> requests;
    bool stopping = false;

    void checkOwner() const { assert(std::this_thread::get_id() == owner && "kast::Network used off its thread"); }

    void emit(std::string name, Json payload)
    {
        const std::lock_guard lock(mutex);
        if (!stopping)
            events.push_back({std::move(name), std::move(payload)});
    }
    template <class Fn>
    void update(const std::string& name, uint64_t serial, Fn&& fn)
    {
        const std::lock_guard lock(mutex);
        if (const auto it = conns.find(name); it != conns.end() && it->second.serial == serial)
            fn(it->second.status);
    }
    void requestDone(uint64_t id)
    {
        const std::lock_guard lock(mutex);
        requests.erase(id);
    }

    std::shared_ptr<ssl::context> tls(const std::string& caPem)
    {
        if (!caPem.empty())
            return makeTlsContext(caPem);
        if (!systemTls)
            systemTls = makeTlsContext({});
        return systemTls;
    }

    void ensureThread()
    {
        if (thread.joinable())
            return;
        io = std::make_unique<asio::io_context>(1);
        work.emplace(io->get_executor());
        thread = std::thread([this] {
            for (;;) {
                try {
                    io->run();
                    return;
                } catch (const std::exception& e) { // a bug in a handler; the rest keep running
                    const std::lock_guard lock(mutex);
                    internalErrors.push_back(printable(e.what()));
                }
            }
        });
    }
};

namespace {

// Resolves host:port, connects stream's TCP layer and, for a TLS stream, sets SNI + hostname verification and
// handshakes. Calls done(error) with an empty error on success. Runs on the io thread.
template <class Stream, class Done>
void connectStream(Stream& stream, tcp::resolver& resolver, const std::string& host, uint16_t port, Done done)
{
    resolver.async_resolve(host, std::to_string(port), [&stream, host, port, done](error_code ec,
                                                                                    tcp::resolver::results_type results) {
        if (ec)
            return done(std::format("cannot resolve '{}': {}", host, ec.message()));
        beast::get_lowest_layer(stream).async_connect(results, [&stream, host, port, done](error_code ec,
                                                                                           const tcp::endpoint&) {
            if (ec)
                return done(std::format("cannot connect to {}:{}: {}", host, port, ec.message()));
            beast::get_lowest_layer(stream).socket().set_option(tcp::no_delay(true), ec);
            if constexpr (std::is_same_v<Stream, TlsStream>) {
                if (!isIpAddress(host))
                    SSL_set_tlsext_host_name(stream.native_handle(), host.c_str());
                stream.set_verify_callback(ssl::host_name_verification(host));
                stream.async_handshake(ssl::stream_base::client, [host, done](error_code ec) {
                    done(ec ? std::format("TLS handshake with {} failed: {}", host, ec.message()) : std::string());
                });
            } else {
                done(std::string());
            }
        });
    });
}

} // namespace

// --- Connections ---

struct Network::Impl::Conn : std::enable_shared_from_this<Conn> {
    Conn(Impl& net, const NetOpenOptions& options, uint64_t serial)
        : net(net), name(options.name), encoding(options.encoding), framing(options.framing), timeout(options.timeout),
          serial(serial), deadline(*net.io), resolver(*net.io)
    {
    }
    virtual ~Conn() = default;

    virtual void start() = 0;
    virtual void write(std::string bytes) = 0; // framed already
    virtual void close() = 0;                  // by script: after queued writes, then net.closed
    virtual void abort() = 0;                  // stop(): no events

    Impl& net;
    std::string name;
    NetEncoding encoding;
    NetFraming framing;
    std::chrono::milliseconds timeout;
    uint64_t serial;
    asio::steady_timer deadline;
    tcp::resolver resolver;
    bool open = false, closed = false, timedOut = false;
    std::string partial; // line / u16le bytes not yet a whole message

    void opened()
    {
        open = true;
        deadline.cancel();
        net.update(name, serial, [](NetConnectionStatus& s) { s.state = NetState::Open; });
        net.emit("net.open", {{"conn", name}});
    }
    // net.closed (after net.error when error isn't empty); later events from this connection are dropped.
    void finish(const std::string& reason, const std::string& error = {})
    {
        if (closed)
            return;
        closed = true;
        deadline.cancel();
        if (!error.empty())
            net.emit("net.error", {{"conn", name}, {"message", printable(error)}});
        net.emit("net.closed", {{"conn", name}, {"reason", printable(reason)}});
        net.update(name, serial, [&](NetConnectionStatus& s) {
            s.state = NetState::Closed;
            if (!error.empty())
                s.lastError = printable(error);
        });
        net.liveConns.erase(shared_from_this());
    }
    void fail(const std::string& error) { finish(error, error); }
    void connectFailed(const std::string& error)
    {
        fail(timedOut ? std::format("timed out after {} ms connecting", timeout.count()) : error);
    }
    // Arms the connect timeout; onExpiry closes the socket so the pending step fails.
    template <class Socket>
    void armTimeout(Socket& socket)
    {
        deadline.expires_after(timeout);
        deadline.async_wait([self = shared_from_this(), &socket](error_code ec) {
            if (ec || self->open || self->closed)
                return;
            self->timedOut = true;
            self->resolver.cancel();
            socket.close();
        });
    }

    void counted(uint64_t in, uint64_t out)
    {
        net.update(name, serial, [&](NetConnectionStatus& s) {
            s.bytesIn += in;
            s.bytesOut += out;
        });
    }
    void deliver(std::string bytes)
    {
        std::string error;
        if (std::optional<Json> data = decode(encoding, std::move(bytes), "received data", error))
            net.emit("net.message", {{"conn", name}, {"data", std::move(*data)}});
        else
            net.emit("net.error", {{"conn", name}, {"message", error}});
    }
    // TCP bytes through the framing: whole messages are delivered, the rest kept in partial.
    void received(std::string_view bytes)
    {
        counted(bytes.size(), 0);
        if (framing == NetFraming::Raw)
            return deliver(std::string(bytes));
        partial.append(bytes);
        size_t at = 0;
        for (;;) {
            if (framing == NetFraming::U16le) {
                if (partial.size() - at < 2)
                    break;
                const size_t length = static_cast<unsigned char>(partial[at]) |
                                      static_cast<unsigned char>(partial[at + 1]) << 8;
                if (partial.size() - at - 2 < length)
                    break;
                deliver(partial.substr(at + 2, length));
                at += 2 + length;
            } else {
                const size_t newline = partial.find('\n', at);
                if (newline == std::string::npos)
                    break;
                deliver(partial.substr(at, newline - at));
                at = newline + 1;
            }
        }
        partial.erase(0, at);
        if (partial.size() > kMaxNetMessage)
            fail(std::format("message exceeds {} bytes", kMaxNetMessage));
    }
};

// One stream type per transport: TcpStream / TlsStream, websocket::stream of either.
template <class Stream> struct TransportOf { using type = Stream; };
template <class Next> struct TransportOf<websocket::stream<Next>> { using type = Next; };

template <class Stream>
struct Network::Impl::Link : Conn {
    using Transport = typename TransportOf<Stream>::type; // TcpStream or TlsStream
    static constexpr bool kWs = !std::is_same_v<Stream, Transport>;

    Link(Impl& net, const NetOpenOptions& options, uint64_t serial, std::string host, uint16_t port, std::string handshakeHost,
         std::string target, const std::shared_ptr<ssl::context>& tls)
        : Conn(net, options, serial), tls(tls), stream(makeStream(net, tls)), host(std::move(host)), port(port),
          handshakeHost(std::move(handshakeHost)), target(std::move(target))
    {
    }

    static Stream makeStream(Impl& net, const std::shared_ptr<ssl::context>& tls)
    {
        if constexpr (std::is_same_v<Transport, TlsStream>)
            return Stream(*net.io, *tls);
        else
            return Stream(*net.io);
    }

    std::shared_ptr<ssl::context> tls; // outlives stream
    Stream stream;
    std::string host;
    uint16_t port;
    std::string handshakeHost, target; // WebSocket only
    std::deque<std::string> outbox;
    bool writing = false, closing = false;
    std::array<char, 64 * 1024> chunk; // a TCP read; also a WebSocket read's size
    beast::flat_buffer message;        // the WebSocket message so far

    Transport& transport()
    {
        if constexpr (kWs)
            return stream.next_layer();
        else
            return stream;
    }
    auto ptr() { return std::static_pointer_cast<Link>(shared_from_this()); }

    void start() override
    {
        armTimeout(beast::get_lowest_layer(stream));
        connectStream(transport(), resolver, host, port, [self = ptr()](const std::string& error) {
            if (self->closed)
                return;
            if (!error.empty())
                return self->connectFailed(error);
            if constexpr (kWs) {
                self->stream.read_message_max(0); // none: read() enforces kMaxNetMessage itself
                self->handshakeTimeout(self->timeout);
                self->stream.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
                    req.set(http::field::user_agent, "Kast");
                }));
                self->stream.async_handshake(self->handshakeHost, self->target, [self](error_code ec) {
                    if (self->closed)
                        return;
                    if (ec)
                        return self->connectFailed(std::format("WebSocket handshake failed: {}", ec.message()));
                    self->connected();
                });
            } else {
                self->connected();
            }
        });
    }
    // Beast's limit on WebSocket opening and closing handshakes; no idle timeout or pings.
    void handshakeTimeout(std::chrono::milliseconds limit)
    {
        websocket::stream_base::timeout limits{};
        limits.handshake_timeout = limit;
        limits.idle_timeout = websocket::stream_base::none();
        limits.keep_alive_pings = false;
        stream.set_option(limits);
    }
    void connected()
    {
        if constexpr (kWs) // a peer that never answers a close frame (message too big, script close) can't hold it open
            handshakeTimeout(std::chrono::seconds(1));
        opened();
        read();
        flush();
    }

    void read()
    {
        if constexpr (kWs) {
            // In pieces, so an oversized message fails at once instead of in a close handshake the peer, still
            // sending, may never answer.
            stream.async_read_some(message, chunk.size(), [self = ptr()](error_code ec, size_t) {
                if (self->closed)
                    return;
                if (ec)
                    return self->readFailed(ec);
                if (self->message.size() > kMaxNetMessage)
                    return self->fail(std::format("message exceeds {} bytes", kMaxNetMessage));
                if (!self->stream.is_message_done())
                    return self->read();
                std::string bytes = beast::buffers_to_string(self->message.data());
                self->message.consume(self->message.size());
                self->counted(bytes.size(), 0);
                self->deliver(std::move(bytes));
                self->read();
            });
        } else {
            stream.async_read_some(asio::buffer(chunk), [self = ptr()](error_code ec, size_t n) {
                if (self->closed)
                    return;
                if (ec)
                    return self->readFailed(ec);
                self->received({self->chunk.data(), n});
                if (!self->closed)
                    self->read();
            });
        }
    }
    void readFailed(error_code ec)
    {
        if (ec == websocket::error::closed || ec == asio::error::eof || ec == asio::error::connection_reset ||
            ec == ssl::error::stream_truncated)
            return finish("closed by peer");
        fail(ec.message());
    }

    void write(std::string bytes) override
    {
        if (closed || closing)
            return;
        outbox.push_back(std::move(bytes));
        if (open && !writing)
            flush();
    }
    void flush()
    {
        if (outbox.empty()) {
            if (closing)
                shutdown();
            return;
        }
        writing = true;
        const auto done = [self = ptr()](error_code ec, size_t n) {
            self->writing = false;
            if (self->closed)
                return;
            if (ec)
                return self->fail(ec.message());
            self->counted(0, n);
            self->outbox.pop_front();
            self->flush();
        };
        if constexpr (kWs) {
            stream.text(encoding != NetEncoding::Base64); // base64 sends raw bytes: binary frames
            stream.async_write(asio::buffer(outbox.front()), done);
        } else {
            asio::async_write(stream, asio::buffer(outbox.front()), done);
        }
    }

    void close() override
    {
        if (closed || closing)
            return;
        closing = true;
        if (!open) // still connecting: nothing to flush
            return shutdown();
        if (!writing)
            flush();
    }
    void shutdown()
    {
        finish("closed by script");
        if constexpr (kWs) { // a close frame, waited for at most a second
            deadline.expires_after(std::chrono::seconds(1));
            deadline.async_wait([self = ptr()](error_code) { beast::get_lowest_layer(self->stream).close(); });
            if (open)
                return stream.async_close(websocket::close_code::normal, [self = ptr()](error_code) {
                    self->deadline.cancel();
                    beast::get_lowest_layer(self->stream).close();
                });
        }
        resolver.cancel();
        error_code ec;
        beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ec);
        beast::get_lowest_layer(stream).close();
    }
    void abort() override
    {
        closed = true;
        deadline.cancel();
        resolver.cancel();
        beast::get_lowest_layer(stream).close();
    }
};

// --- HTTP requests ---

struct Network::Impl::Request : std::enable_shared_from_this<Request> {
    Request(Impl& net, uint64_t id) : net(net), id(id), deadline(*net.io), resolver(*net.io) {}
    virtual ~Request() = default;
    virtual void start() = 0;
    virtual void abort() = 0;

    Impl& net;
    uint64_t id;
    asio::steady_timer deadline;
    tcp::resolver resolver;
    bool done = false, timedOut = false;

    void finish(std::string name, Json payload)
    {
        if (done)
            return;
        done = true;
        deadline.cancel();
        payload["id"] = id;
        net.emit(std::move(name), std::move(payload));
        net.requestDone(id);
        net.liveRequests.erase(shared_from_this());
    }
    void fail(const std::string& message) { finish("net.http.error", {{"message", printable(message)}}); }
};

template <class Stream>
struct Network::Impl::HttpRequest : Request {
    HttpRequest(Impl& net, uint64_t id, NetRequestOptions options, NetUrl url, const std::shared_ptr<ssl::context>& tls)
        : Request(net, id), options(std::move(options)), url(std::move(url)), tls(tls), stream(makeStream(net, tls))
    {
    }
    static Stream makeStream(Impl& net, const std::shared_ptr<ssl::context>& tls)
    {
        if constexpr (std::is_same_v<Stream, TlsStream>)
            return Stream(*net.io, *tls);
        else
            return Stream(*net.io);
    }

    NetRequestOptions options;
    NetUrl url;
    std::shared_ptr<ssl::context> tls;
    Stream stream;
    http::request<http::string_body> req;
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    auto ptr() { return std::static_pointer_cast<HttpRequest>(shared_from_this()); }

    void start() override
    {
        deadline.expires_after(options.timeout);
        deadline.async_wait([self = ptr()](error_code ec) {
            if (ec || self->done)
                return;
            self->timedOut = true;
            self->resolver.cancel();
            beast::get_lowest_layer(self->stream).close();
        });
        connectStream(stream, resolver, url.host, url.port, [self = ptr()](const std::string& error) {
            if (self->done)
                return;
            if (!error.empty())
                return self->failed(error);
            self->send();
        });
    }
    void failed(const std::string& error)
    {
        fail(timedOut ? std::format("request timed out after {} ms", options.timeout.count()) : error);
    }
    void send()
    {
        req.method_string(options.method);
        req.target(url.target);
        req.version(11);
        req.set(http::field::host, hostHeader(url));
        req.set(http::field::user_agent, "Kast");
        for (const auto& [name, value] : options.headers)
            req.set(name, value);
        req.body() = std::move(options.body);
        req.prepare_payload();
        http::async_write(stream, req, [self = ptr()](error_code ec, size_t) {
            if (self->done)
                return;
            if (ec)
                return self->failed(ec.message());
            self->parser.body_limit(kMaxNetMessage);
            http::async_read(self->stream, self->buffer, self->parser, [self](error_code ec, size_t) {
                if (self->done)
                    return;
                if (ec == http::error::body_limit)
                    return self->fail(std::format("response body exceeds {} bytes", kMaxNetMessage));
                if (ec)
                    return self->failed(ec.message());
                self->respond();
            });
        });
    }
    void respond()
    {
        auto& res = parser.get();
        Json headers = Json::object();
        for (const auto& field : res) {
            std::string name(field.name_string()), value(field.value());
            if (!isUtf8(name) || !isUtf8(value))
                return fail("a response header is not valid UTF-8");
            Json& slot = headers[name];
            slot = slot.is_null() ? Json(value) : Json(slot.get<std::string>() + ", " + value);
        }
        std::string error;
        std::optional<Json> body = decode(options.encoding, std::move(res.body()), "response body", error);
        if (!body)
            return fail(error);
        const unsigned status = res.result_int();
        finish("net.http.response", {{"status", status}, {"headers", std::move(headers)}, {"body", std::move(*body)}});
        beast::get_lowest_layer(stream).close();
    }
    void abort() override
    {
        done = true;
        deadline.cancel();
        resolver.cancel();
        beast::get_lowest_layer(stream).close();
    }
};

// --- Free functions ---

const char* netStateName(NetState state)
{
    switch (state) {
    case NetState::Connecting:
        return "connecting";
    case NetState::Open:
        return "open";
    case NetState::Closed:
        return "closed";
    }
    return "closed";
}

std::optional<NetUrl> parseNetUrl(std::string_view url, std::initializer_list<std::string_view> schemes,
                                  std::string& error)
{
    const size_t sep = url.find("://");
    std::string scheme(sep == std::string_view::npos ? std::string_view() : url.substr(0, sep));
    for (char& c : scheme)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (std::find(schemes.begin(), schemes.end(), scheme) == schemes.end()) {
        std::string expected;
        for (std::string_view s : schemes)
            expected += std::format("{}{}://", expected.empty() ? "" : " or ", s);
        error = std::format("url must start with {}", expected);
        return std::nullopt;
    }
    std::string_view rest = url.substr(sep + 3);
    // ponytail: no [IPv6] literals or user@ parts; add when a game needs them.
    const size_t end = std::min(rest.find_first_of("/?#"), rest.size());
    std::string_view authority = rest.substr(0, end);
    std::string target(rest.substr(end));
    if (const size_t hash = target.find('#'); hash != std::string::npos)
        target.erase(hash);
    if (target.empty() || target[0] != '/')
        target.insert(0, "/");
    uint16_t port = (scheme == "https" || scheme == "wss") ? 443 : 80;
    if (const size_t colon = authority.find(':'); colon != std::string_view::npos) {
        const std::string_view digits = authority.substr(colon + 1);
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (ec != std::errc{} || ptr != digits.data() + digits.size() || value == 0 || value > 65535) {
            error = std::format("invalid port in url '{}'", url);
            return std::nullopt;
        }
        port = static_cast<uint16_t>(value);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        error = std::format("url '{}' has no host", url);
        return std::nullopt;
    }
    return NetUrl{scheme, std::string(authority), port, target};
}

std::string base64Encode(std::string_view bytes)
{
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), reinterpret_cast<const unsigned char*>(bytes.data()),
                    static_cast<int>(bytes.size()));
    return out;
}

std::optional<std::string> base64Decode(std::string_view text)
{
    // EVP_DecodeBlock skips whitespace and accepts '=' anywhere; require the strict form first.
    if (text.size() % 4 != 0)
        return std::nullopt;
    const size_t padding = text.ends_with("==") ? 2 : text.ends_with('=') ? 1 : 0;
    for (size_t i = 0; i < text.size() - padding; ++i) {
        const char c = text[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '/')
            return std::nullopt;
    }
    std::string out(text.size() / 4 * 3, '\0');
    const int n = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                  reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    if (n < 0)
        return std::nullopt;
    out.resize(static_cast<size_t>(n) - padding);
    return out;
}

bool isUtf8(std::string_view text)
{
    // ponytail: copies the text; exactly what the EventBus accepts (it dumps every payload). Hand-roll if it shows up
    // in profiles.
    try {
        (void)Json(std::string(text)).dump();
        return true;
    } catch (const Json::type_error&) {
        return false;
    }
}

// --- Network ---

Network::Network() : impl_(std::make_unique<Impl>()) {}

Network::~Network() { stop(); }

void Network::open(NetOpenOptions options)
{
    Impl& n = *impl_;
    n.checkOwner();
    if (options.name.empty())
        throw std::invalid_argument("name must be a non-empty string");
    {
        const std::lock_guard lock(n.mutex);
        if (const auto it = n.conns.find(options.name); it != n.conns.end() && it->second.status.state != NetState::Closed)
            throw std::invalid_argument(std::format("connection '{}' is already open", options.name));
    }
    std::string host, handshakeHost, target, address;
    uint16_t port = 0;
    bool tls = options.tls;
    if (options.type == NetType::Tcp) {
        if (options.host.empty())
            throw std::invalid_argument("host must be a non-empty string");
        if (options.port == 0)
            throw std::invalid_argument("port must be 1..65535");
        host = options.host;
        port = options.port;
        address = std::format("{}:{}", host, port);
    } else {
        std::string error;
        const std::optional<NetUrl> url = parseNetUrl(options.url, {"ws", "wss"}, error);
        if (!url)
            throw std::invalid_argument(error);
        host = url->host;
        port = url->port;
        tls = url->scheme == "wss";
        handshakeHost = hostHeader(*url);
        target = url->target;
        address = options.url;
    }
    std::shared_ptr<ssl::context> context = tls ? n.tls(options.caPem) : nullptr;
    n.ensureThread();
    const uint64_t serial = n.nextSerial++;
    std::shared_ptr<Impl::Conn> conn;
    const auto make = [&]<class Stream>() {
        conn = std::make_shared<Impl::Link<Stream>>(n, options, serial, host, port, handshakeHost, target, context);
    };
    if (options.type == NetType::Tcp)
        tls ? make.operator()<TlsStream>() : make.operator()<TcpStream>();
    else
        tls ? make.operator()<websocket::stream<TlsStream>>() : make.operator()<websocket::stream<TcpStream>>();
    {
        const std::lock_guard lock(n.mutex);
        n.conns[options.name] = {
            .status = {options.name, options.type, address, tls, NetState::Connecting, 0, 0, {}},
            .encoding = options.encoding,
            .framing = options.type == NetType::Tcp ? options.framing : NetFraming::Raw,
            .serial = serial,
            .conn = conn,
        };
    }
    asio::post(*n.io, [&n, conn] {
        n.liveConns.insert(conn);
        conn->start();
    });
}

void Network::send(const std::string& name, std::string bytes)
{
    Impl& n = *impl_;
    n.checkOwner();
    std::shared_ptr<Impl::Conn> conn;
    NetFraming framing;
    {
        const std::lock_guard lock(n.mutex);
        const auto it = n.conns.find(name);
        if (it == n.conns.end())
            throw std::invalid_argument(std::format("no connection named '{}'", name));
        if (it->second.status.state == NetState::Closed)
            throw std::invalid_argument(std::format("connection '{}' is closed", name));
        conn = it->second.conn;
        framing = it->second.framing;
    }
    if (bytes.size() > kMaxNetMessage)
        throw std::invalid_argument(std::format("message is {} bytes, the limit is {}", bytes.size(), kMaxNetMessage));
    if (framing == NetFraming::U16le) {
        if (bytes.size() > 0xFFFF)
            throw std::invalid_argument(std::format("message is {} bytes, u16le framing allows 65535", bytes.size()));
        const char prefix[2] = {static_cast<char>(bytes.size() & 0xFF), static_cast<char>(bytes.size() >> 8)};
        bytes.insert(0, prefix, 2);
    } else if (framing == NetFraming::Line) {
        if (bytes.find('\n') != std::string::npos)
            throw std::invalid_argument("message contains a newline, which line framing uses to end it");
        bytes.push_back('\n');
    }
    asio::post(*n.io, [conn, bytes = std::move(bytes)]() mutable { conn->write(std::move(bytes)); });
}

void Network::close(const std::string& name)
{
    Impl& n = *impl_;
    n.checkOwner();
    std::shared_ptr<Impl::Conn> conn;
    {
        const std::lock_guard lock(n.mutex);
        const auto it = n.conns.find(name);
        if (it == n.conns.end())
            return;
        conn = std::move(it->second.conn);
        n.conns.erase(it);
    }
    asio::post(*n.io, [conn] { conn->close(); });
}

uint64_t Network::request(NetRequestOptions options)
{
    Impl& n = *impl_;
    n.checkOwner();
    std::string error;
    std::optional<NetUrl> url = parseNetUrl(options.url, {"http", "https"}, error);
    if (!url)
        throw std::invalid_argument(error);
    if (options.method.empty() || options.method.find_first_of(" \t\r\n") != std::string::npos)
        throw std::invalid_argument(std::format("invalid method '{}'", options.method));
    for (const auto& [name, value] : options.headers)
        if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos)
            throw std::invalid_argument(std::format("invalid header '{}'", name));
    const bool tls = url->scheme == "https";
    std::shared_ptr<ssl::context> context = tls ? n.tls(options.caPem) : nullptr;
    n.ensureThread();
    const uint64_t id = n.nextRequest++;
    {
        const std::lock_guard lock(n.mutex);
        n.requests[id] = {id, options.method, options.url};
    }
    std::shared_ptr<Impl::Request> request;
    if (tls)
        request = std::make_shared<Impl::HttpRequest<TlsStream>>(n, id, std::move(options), std::move(*url), context);
    else
        request = std::make_shared<Impl::HttpRequest<TcpStream>>(n, id, std::move(options), std::move(*url), context);
    asio::post(*n.io, [&n, request] {
        n.liveRequests.insert(request);
        request->start();
    });
    return id;
}

bool Network::has(const std::string& name) const
{
    impl_->checkOwner();
    const std::lock_guard lock(impl_->mutex);
    return impl_->conns.contains(name);
}

std::optional<NetState> Network::state(const std::string& name) const
{
    impl_->checkOwner();
    const std::lock_guard lock(impl_->mutex);
    const auto it = impl_->conns.find(name);
    return it == impl_->conns.end() ? std::nullopt : std::optional(it->second.status.state);
}

std::optional<NetEncoding> Network::encoding(const std::string& name) const
{
    impl_->checkOwner();
    const std::lock_guard lock(impl_->mutex);
    const auto it = impl_->conns.find(name);
    return it == impl_->conns.end() ? std::nullopt : std::optional(it->second.encoding);
}

std::vector<NetEvent> Network::poll()
{
    Impl& n = *impl_;
    n.checkOwner();
    std::vector<NetEvent> out;
    std::vector<std::string> errors;
    {
        const std::lock_guard lock(n.mutex);
        out.swap(n.events);
        errors.swap(n.internalErrors);
    }
    for (const std::string& error : errors)
        log::error(std::format("Network: {}", error));
    return out;
}

void Network::stop()
{
    Impl& n = *impl_;
    n.checkOwner();
    if (n.thread.joinable()) {
        {
            const std::lock_guard lock(n.mutex);
            n.stopping = true;
        }
        asio::post(*n.io, [&n] {
            for (const auto& conn : std::exchange(n.liveConns, {}))
                conn->abort();
            for (const auto& request : std::exchange(n.liveRequests, {}))
                request->abort();
        });
        n.work.reset();
        // ponytail: an in-progress DNS lookup can't be interrupted, so join waits for it (the OS's resolver timeout).
        n.thread.join();
    }
    const std::lock_guard lock(n.mutex);
    n.conns.clear(); // the connections go before io, whose timers and sockets they hold
    n.requests.clear();
    n.events.clear();
    n.stopping = false;
    n.io.reset();
}

NetStatus Network::status() const
{
    const std::lock_guard lock(impl_->mutex);
    NetStatus out;
    for (const auto& [name, entry] : impl_->conns)
        out.connections.push_back(entry.status);
    for (const auto& [id, request] : impl_->requests)
        out.requests.push_back(request);
    return out;
}

} // namespace kast
