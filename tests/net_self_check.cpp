// kast::Network, kast.net, kast::AuthClient and kast.auth checks for kast_self_check, against loopback servers (tests/net_test_util.h) on ephemeral
// ports with certificates made at run time. Kept out of self_check.cpp so only this file compiles Boost.Asio / Beast.
#include "net_test_util.h"

#include "auth_client.h"
#include "document.h"
#include "log.h"
#include "lua_systems.h"
#include "network.h"
#include "project.h"
#include "scene_io.h"
#include "world_client.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <process.h>
#include <fstream>
#include <source_location>
#include <thread>
#include <unordered_map>

namespace {

using namespace kast::nettest;
using kast::NetEncoding;
using kast::NetEvent;
using kast::NetFraming;
using kast::NetOpenOptions;
using kast::NetRequestOptions;
using kast::NetState;
using kast::NetType;
using kast::Network;
using Json = nlohmann::json;
namespace proto = kast::proto;
using namespace std::chrono_literals;

int g_failures = 0;

void check(bool ok, const std::string& what, std::source_location where = std::source_location::current())
{
    if (ok)
        return;
    ++g_failures;
    kast::log::error(std::format("FAILED net_self_check.cpp line {}: {}", where.line(), what));
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

bool logged(const std::string& part)
{
    return std::ranges::any_of(kast::log::lines(), [&](const std::string& line) { return contains(line, part); });
}

// What a Network delivered so far, polled on this (its owner) thread.
struct Events {
    Network& net;
    std::vector<NetEvent> all;

    // Polls until done() or timeout; false on timeout.
    bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            for (NetEvent& e : net.poll())
                all.push_back(std::move(e));
            if (done())
                return true;
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(1ms);
        }
    }
    std::vector<const Json*> of(const std::string& name, const std::string& conn) const
    {
        std::vector<const Json*> out;
        for (const NetEvent& e : all)
            if (e.name == name && e.payload.value("conn", "") == conn)
                out.push_back(&e.payload);
        return out;
    }
    std::vector<Json> messages(const std::string& conn) const
    {
        std::vector<Json> out;
        for (const Json* p : of("net.message", conn))
            out.push_back(p->at("data"));
        return out;
    }
    bool has(const std::string& name, const std::string& conn) const { return !of(name, conn).empty(); }
    std::string error(const std::string& conn) const
    {
        const auto errors = of("net.error", conn);
        return errors.empty() ? std::string() : errors.back()->at("message").get<std::string>();
    }
    const Json* response(uint64_t id, const char* name = "net.http.response") const
    {
        for (const NetEvent& e : all)
            if (e.name == name && e.payload.value("id", uint64_t(0)) == id)
                return &e.payload;
        return nullptr;
    }
    bool closed(const std::string& conn) const { return has("net.closed", conn); }
};

NetOpenOptions tcp(const std::string& name, uint16_t port, NetFraming framing = NetFraming::Raw,
                   NetEncoding encoding = NetEncoding::Text)
{
    return {.name = name, .type = NetType::Tcp, .host = "127.0.0.1", .port = port, .encoding = encoding,
            .framing = framing};
}

NetOpenOptions ws(const std::string& name, const std::string& url, NetEncoding encoding = NetEncoding::Text,
                  const std::string& caPem = {})
{
    return {.name = name, .type = NetType::Ws, .url = url, .caPem = caPem, .encoding = encoding};
}

std::string u16le(const std::string& payload)
{
    return std::string{static_cast<char>(payload.size() & 0xFF), static_cast<char>(payload.size() >> 8)} + payload;
}

// Blocks until the client closes (EOF or reset).
template <class Stream>
void drainUntilClosed(Stream& stream)
{
    char byte;
    boost::system::error_code ec;
    while (!ec)
        stream.read_some(asio::buffer(&byte, 1), ec);
}

bool throwsInvalid(const std::function<void()>& fn, const std::string& part = {})
{
    try {
        fn();
    } catch (const std::invalid_argument& e) {
        return part.empty() || contains(e.what(), part);
    }
    return false;
}

// --- Network (C++) ---

void checkNetHelpers()
{
    check(kast::base64Encode("") == "" && kast::base64Encode(std::string("\x00\xff\x10", 3)) == "AP8Q" &&
              kast::base64Encode("ab") == "YWI=",
          "base64Encode");
    check(kast::base64Decode("AP8Q") == std::string("\x00\xff\x10", 3) && kast::base64Decode("YWI=") == "ab" &&
              kast::base64Decode("") == "",
          "base64Decode");
    check(!kast::base64Decode("abc") && !kast::base64Decode("ab=c") && !kast::base64Decode("a b=") &&
              !kast::base64Decode("YW I="),
          "base64Decode rejects bad input");
    check(kast::isUtf8("h\xc3\xa9llo") && !kast::isUtf8("\xff") && !kast::isUtf8("\xc0\xaf") && !kast::isUtf8("\xed\xa0\x80"),
          "isUtf8 (overlong and surrogate forms are invalid)");
    std::string error;
    const auto url = kast::parseNetUrl("WSS://Example.com:8443/a?b#frag", {"ws", "wss"}, error);
    check(url && url->scheme == "wss" && url->host == "Example.com" && url->port == 8443 && url->target == "/a?b",
          "parseNetUrl: scheme, host, port, target");
    const auto bare = kast::parseNetUrl("http://h?q=1", {"http", "https"}, error);
    check(bare && bare->port == 80 && bare->target == "/?q=1", "parseNetUrl: default port, query-only target");
    check(!kast::parseNetUrl("http://x", {"ws", "wss"}, error) && contains(error, "ws:// or wss://"),
          "parseNetUrl: wrong scheme");
    check(!kast::parseNetUrl("ws:///x", {"ws"}, error) && !kast::parseNetUrl("ws://h:0/", {"ws"}, error) &&
              !kast::parseNetUrl("ws://h:70000/", {"ws"}, error),
          "parseNetUrl: no host, bad ports");
}

void checkNetTcpFraming()
{
    Network net;
    Events ev{net};

    Server raw([](Server& s) {
        tcp::socket socket = s.accept();
        std::string buffer;
        const std::string line = readLine(socket, buffer); // the client's raw send, which carries its own newline
        asio::write(socket, asio::buffer("echo " + line));
        drainUntilClosed(socket);
    });
    Server framed([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(u16le("one") + u16le("two"))); // coalesced
        const std::string three = u16le("three");
        asio::write(socket, asio::buffer(three.substr(0, 1))); // split inside the length prefix
        std::this_thread::sleep_for(50ms);
        asio::write(socket, asio::buffer(three.substr(1, 3)));
        std::this_thread::sleep_for(50ms);
        asio::write(socket, asio::buffer(three.substr(4)));
        unsigned char prefix[2];
        asio::read(socket, asio::buffer(prefix));
        std::string payload(prefix[0] | prefix[1] << 8, '\0');
        asio::read(socket, asio::buffer(payload));
        asio::write(socket, asio::buffer(u16le(payload == "ping" ? "pong" : "bad:" + payload) + u16le("")));
        drainUntilClosed(socket);
    });
    Server lines([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(std::string("a\nb\n"))); // coalesced
        asio::write(socket, asio::buffer(std::string("c")));      // split
        std::this_thread::sleep_for(50ms);
        asio::write(socket, asio::buffer(std::string("d\n")));
        std::string buffer;
        const std::string line = readLine(socket, buffer);
        asio::write(socket, asio::buffer("ok " + line + "\n"));
        drainUntilClosed(socket);
    });

    net.open(tcp("raw", raw.port()));
    net.open(tcp("u16", framed.port(), NetFraming::U16le));
    net.open(tcp("line", lines.port(), NetFraming::Line));
    check(net.state("raw") == NetState::Connecting, "a new connection is connecting");
    net.send("raw", "hi\n"); // queued until open
    check(ev.waitFor([&] { return ev.messages("u16").size() == 3; }), "u16le: three frames arrive");
    net.send("u16", "ping");
    net.send("line", "x");
    check(ev.waitFor([&] {
              return ev.messages("raw").size() == 1 && ev.messages("u16").size() == 5 && ev.messages("line").size() == 4;
          }),
          "raw, u16le and line replies arrive");
    check(ev.has("net.open", "raw") && net.state("raw") == NetState::Open, "net.open, then state open");
    check(ev.messages("raw") == std::vector<Json>{"echo hi"}, "raw: data sent while connecting goes out on open");
    check(ev.messages("u16") == std::vector<Json>{"one", "two", "three", "pong", ""},
          "u16le: coalesced and split frames, the send prefix, an empty frame");
    check(ev.messages("line") == std::vector<Json>{"a", "b", "cd", "ok x"}, "line: coalesced, split, send appends \\n");
    check(throwsInvalid([&] { net.send("u16", std::string(65536, 'x')); }, "65535"), "u16le: send over 65535 throws");
    check(throwsInvalid([&] { net.send("line", "a\nb"); }, "newline"), "line: send with a newline throws");
    check(throwsInvalid([&] { net.send("raw", std::string(kast::kMaxNetMessage + 1, 'x')); }, "limit"),
          "send over 16 MiB throws");
    check(throwsInvalid([&] { net.send("nope", "x"); }, "no connection"), "send to an unknown name throws");

    const kast::NetStatus status = net.status();
    check(status.connections.size() == 3 && status.connections[0].name == "line" &&
              status.connections[0].address == std::format("127.0.0.1:{}", lines.port()) &&
              status.connections[0].bytesIn == 12 && status.connections[0].bytesOut == 2,
          "status: name, address, bytes in/out");
    net.stop();
}

void checkNetEncodings()
{
    Network net;
    Events ev{net};
    Server text([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(std::string("\xff\xfe\nok\n")));
        drainUntilClosed(socket);
    });
    Server json([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(std::string("{bad\n[1,{\"a\":\"b\"}]\n")));
        std::string buffer;
        asio::write(socket, asio::buffer(readLine(socket, buffer) + "\n")); // echo
        drainUntilClosed(socket);
    });
    Server binary([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(std::string("\x00\xff\x01", 3)));
        char bytes[3];
        asio::read(socket, asio::buffer(bytes));
        asio::write(socket, asio::buffer(bytes)); // echo
        drainUntilClosed(socket);
    });
    net.open(tcp("text", text.port(), NetFraming::Line));
    net.open(tcp("json", json.port(), NetFraming::Line, NetEncoding::Json));
    net.open(tcp("b64", binary.port(), NetFraming::Raw, NetEncoding::Base64));
    check(ev.waitFor([&] { return ev.messages("text").size() == 1 && ev.messages("json").size() == 1 &&
                                  ev.messages("b64").size() == 1; }),
          "encodings: first messages arrive");
    check(contains(ev.error("text"), "not valid UTF-8") && ev.messages("text") == std::vector<Json>{"ok"} &&
              net.state("text") == NetState::Open,
          "text: invalid UTF-8 is a net.error; the connection stays open");
    check(contains(ev.error("json"), "not valid JSON") && ev.messages("json")[0] == Json::parse(R"([1,{"a":"b"}])"),
          "json: bad JSON is a net.error, good JSON parses");
    check(ev.messages("b64")[0] == "AP8B", "base64: received bytes arrive as base64");
    net.send("b64", *kast::base64Decode("AAEC"));
    net.send("json", Json{{"k", 2}}.dump());
    check(ev.waitFor([&] { return ev.messages("b64").size() == 2 && ev.messages("json").size() == 2; }), "echoes arrive");
    check(ev.messages("b64")[1] == "AAEC", "base64: raw bytes round-trip");
    check(ev.messages("json")[1] == Json{{"k", 2}}, "json: sent JSON round-trips");
    net.stop();
}

void checkNetTls()
{
    const Cert good = makeCert("DNS:localhost,IP:127.0.0.1");
    const Cert other = makeCert("DNS:other.example");
    Network net;
    Events ev{net};
    const auto echo = [](Server& s) {
        TlsSocket stream = s.acceptTls();
        std::string buffer;
        asio::write(stream, asio::buffer(readLine(stream, buffer) + "\n"));
        drainUntilClosed(stream);
    };
    Server ok(echo, 0, &good), untrusted(echo, 0, &good), mismatch(echo, 0, &other);
    NetOpenOptions trusted = tcp("tls", ok.port(), NetFraming::Line);
    trusted.tls = true;
    trusted.host = "localhost"; // SNI + DNS name check
    trusted.caPem = good.certPem;
    NetOpenOptions plain = tcp("untrusted", untrusted.port(), NetFraming::Line);
    plain.tls = true;
    NetOpenOptions wrongHost = tcp("mismatch", mismatch.port(), NetFraming::Line);
    wrongHost.tls = true;
    wrongHost.caPem = other.certPem; // trusted, but issued for other.example
    net.open(trusted);
    net.open(plain);
    net.open(wrongHost);
    net.send("tls", "secret");
    check(ev.waitFor([&] {
              return ev.messages("tls").size() == 1 && ev.closed("untrusted") && ev.closed("mismatch");
          }),
          "TLS: results arrive");
    check(ev.messages("tls") == std::vector<Json>{"secret"}, "TLS: trusted via caPem, hostname localhost verifies");
    check(contains(ev.error("untrusted"), "TLS handshake") && contains(ev.error("untrusted"), "certificate verify failed"),
          "TLS: a self-signed server without caFile fails verification: " + ev.error("untrusted"));
    check(contains(ev.error("mismatch"), "certificate verify failed"),
          "TLS: a trusted certificate for another host fails: " + ev.error("mismatch"));
    check(net.state("untrusted") == NetState::Closed && net.status().connections[0].lastError == ev.error("mismatch"),
          "TLS: failures close and record lastError");
    check(throwsInvalid([&] {
              NetOpenOptions bad = tcp("bad", ok.port());
              bad.tls = true;
              bad.caPem = "not a certificate";
              net.open(bad);
          }, "caFile"),
          "TLS: an unparsable caPem throws");
    net.stop();
}

void checkNetWebSocket()
{
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    Network net;
    Events ev{net};
    const auto echo = [](auto ws) {
        ws.accept();
        if constexpr (std::is_same_v<decltype(ws), websocket::stream<tcp::socket>>) {
            ws.binary(true);
            ws.write(asio::buffer(std::string("\x00\xff\x01", 3)));
        }
        for (;;) {
            beast::flat_buffer buffer;
            ws.read(buffer);
            ws.text(ws.got_text());
            ws.write(buffer.data());
        }
    };
    Server plain([&](Server& s) { echo(websocket::stream<tcp::socket>(s.accept())); });
    Server secure([&](Server& s) { echo(websocket::stream<TlsSocket>(s.acceptTls())); }, 0, &cert);
    net.open(ws("ws", std::format("ws://127.0.0.1:{}/chat", plain.port()), NetEncoding::Base64));
    net.open(ws("wss", std::format("wss://localhost:{}/", secure.port()), NetEncoding::Json, cert.certPem));
    check(ev.waitFor([&] { return ev.messages("ws").size() == 1 && ev.has("net.open", "wss"); }), "ws, wss open");
    check(ev.messages("ws")[0] == "AP8B", "ws: a binary message arrives as base64");
    net.send("ws", std::string("\x00\x01\x02", 3));
    net.send("wss", R"({"hello":"wss"})");
    check(ev.waitFor([&] { return ev.messages("ws").size() == 2 && ev.messages("wss").size() == 1; }), "ws echoes");
    check(ev.messages("ws")[1] == "AAEC", "ws: base64 sends binary frames, which round-trip");
    check(ev.messages("wss")[0] == Json{{"hello", "wss"}}, "wss: json encoding round-trips");
    check(net.status().connections[1].tls && net.status().connections[1].address.starts_with("wss://"),
          "status: wss is tls, address is the url");
    net.close("wss");
    check(!net.has("wss") && ev.waitFor([&] { return ev.closed("wss"); }), "close: forgets the name, then net.closed");
    check(ev.of("net.closed", "wss")[0]->at("reason") == "closed by script", "close: reason 'closed by script'");
    net.stop();
}

void checkNetHttp()
{
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    Network net;
    Events ev{net};
    const auto serve = [](auto& stream) {
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(stream, buffer, req);
        http::response<http::string_body> res{http::status::created, 11};
        res.set("X-Echo", req["X-Test"]);
        res.set("X-Method", req.method_string());
        res.body() = req.target() == "/json" ? R"({"x":[1,2]})" : req.body();
        res.prepare_payload();
        http::write(stream, res);
    };
    Server plain([&](Server& s) {
        tcp::socket socket = s.accept();
        serve(socket);
    });
    Server secure([&](Server& s) {
        TlsSocket stream = s.acceptTls();
        serve(stream);
    }, 0, &cert);
    const uint64_t post = net.request({.method = "POST",
                                       .url = std::format("http://127.0.0.1:{}/echo", plain.port()),
                                       .headers = {{"X-Test", "yes"}},
                                       .body = "payload"});
    const uint64_t get = net.request({.url = std::format("https://localhost:{}/json", secure.port()),
                                      .caPem = cert.certPem,
                                      .encoding = NetEncoding::Json});
    check(post == 1 && get == 2, std::format("request ids from 1, got {} and {}", post, get));
    check(ev.waitFor([&] { return ev.response(post) && ev.response(get); }), "HTTP, HTTPS responses arrive");
    if (const Json* r = ev.response(post))
        check(r->at("status") == 201 && r->at("headers").at("X-Echo") == "yes" && r->at("headers").at("X-Method") == "POST" &&
                  r->at("body") == "payload",
              "HTTP: method, header and body reach the server; status, headers, body come back");
    if (const Json* r = ev.response(get))
        check(r->at("body") == Json::parse(R"({"x":[1,2]})"), "HTTPS: json encoding parses the body");
    check(net.status().requests.empty(), "finished requests leave the status");
    check(throwsInvalid([&] { net.request({.url = "ftp://x"}); }, "http:// or https://"), "request: bad scheme throws");
    check(throwsInvalid([&] { net.request({.url = "http://x", .headers = {{"A\r\nB", "c"}}}); }, "header"),
          "request: a header with CR/LF throws");
    net.stop();
}

void checkNetErrors()
{
    Network net;
    Events ev{net};
    uint16_t refusedPort;
    {
        asio::io_context io;
        tcp::acceptor probe(io, {asio::ip::make_address("127.0.0.1"), 0});
        refusedPort = probe.local_endpoint().port();
    }
    Server stall([](Server& s) { // accepts TCP, never speaks TLS or HTTP
        tcp::socket a = s.accept();
        tcp::socket b = s.accept();
        drainUntilClosed(a);
        drainUntilClosed(b);
    });
    Server hangup([](Server& s) { s.accept().close(); });
    Server huge([](Server& s) {
        tcp::socket socket = s.accept();
        asio::write(socket, asio::buffer(std::string(kast::kMaxNetMessage + 16, 'x'))); // one line that never ends
        drainUntilClosed(socket);
    });
    Server hugeWs([](Server& s) {
        websocket::stream<tcp::socket> stream(s.accept());
        stream.accept();
        stream.write(asio::buffer(std::string(kast::kMaxNetMessage + 1, 'x')));
        drainUntilClosed(stream.next_layer());
    });
    Server hugeHttp([](Server& s) {
        tcp::socket socket = s.accept();
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(socket, buffer, req);
        http::response<http::string_body> res{http::status::ok, 11};
        res.body() = std::string(kast::kMaxNetMessage + 1, 'x');
        res.prepare_payload();
        boost::system::error_code ec;
        http::write(socket, res, ec);
    });

    net.open(tcp("refused", refusedPort));
    NetOpenOptions dns = tcp("dns", 80);
    dns.host = "kast-does-not-exist.invalid";
    net.open(dns);
    NetOpenOptions slow = tcp("timeout", stall.port());
    slow.tls = true;
    slow.timeout = 300ms;
    net.open(slow);
    net.open(tcp("hangup", hangup.port()));
    net.open(tcp("huge", huge.port(), NetFraming::Line));
    net.open(ws("hugews", std::format("ws://127.0.0.1:{}/", hugeWs.port())));
    const uint64_t timedOut = net.request({.url = std::format("http://127.0.0.1:{}/", stall.port()), .timeout = 300ms});
    const uint64_t bigBody = net.request({.url = std::format("http://127.0.0.1:{}/", hugeHttp.port())});
    check(ev.waitFor([&] {
              return ev.closed("refused") && ev.closed("dns") && ev.closed("timeout") && ev.closed("hangup") &&
                     ev.closed("huge") && ev.closed("hugews") && ev.response(timedOut, "net.http.error") &&
                     ev.response(bigBody, "net.http.error");
          }, 15s),
          "every error path ends in events");
    check(contains(ev.error("refused"), "cannot connect"), "refused: " + ev.error("refused"));
    check(contains(ev.error("dns"), "cannot resolve"), "DNS failure: " + ev.error("dns"));
    check(contains(ev.error("timeout"), "timed out"), "connect timeout: " + ev.error("timeout"));
    check(ev.error("hangup").empty() && ev.of("net.closed", "hangup")[0]->at("reason") == "closed by peer",
          "remote close: net.closed 'closed by peer', no net.error");
    check(contains(ev.error("huge"), "exceeds"), "line over 16 MiB: " + ev.error("huge"));
    check(contains(ev.error("hugews"), "exceeds"), "ws message over 16 MiB: " + ev.error("hugews"));
    if (const Json* e = ev.response(timedOut, "net.http.error"))
        check(contains(e->at("message"), "timed out"), "request timeout: " + e->at("message").get<std::string>());
    if (const Json* e = ev.response(bigBody, "net.http.error"))
        check(contains(e->at("message"), "exceeds"), "body over 16 MiB: " + e->at("message").get<std::string>());

    check(throwsInvalid([&] { net.open(tcp("", 1)); }, "name"), "open: empty name throws");
    check(throwsInvalid([&] { net.open(tcp("p", 0)); }, "port"), "open: port 0 throws");
    check(throwsInvalid([&] { net.open(ws("w", "http://x")); }, "ws://"), "open: bad ws url throws");
    net.open(tcp("dup", stall.port()));
    check(throwsInvalid([&] { net.open(tcp("dup", stall.port())); }, "already open"), "open: a duplicate name throws");
    check(!throwsInvalid([&] { net.open(tcp("refused", refusedPort)); }), "open: a closed name can be reopened");
    check(throwsInvalid([&] { net.send("hangup", "x"); }, "closed"), "send on a closed connection throws");
    net.stop();
}

void checkNetStop()
{
    std::atomic<int> observed = 0;
    Server held([&](Server& s) {
        tcp::socket socket = s.accept();
        drainUntilClosed(socket);
        ++observed;
    });
    Server slow([&](Server& s) {
        tcp::socket socket = s.accept();
        drainUntilClosed(socket); // never answers
        ++observed;
    });
    Network net;
    Events ev{net};
    net.open(tcp("held", held.port()));
    net.request({.url = std::format("http://127.0.0.1:{}/", slow.port())});
    check(ev.waitFor([&] { return ev.has("net.open", "held") && slow.accepted() == 1; }), "stop: connected");
    check(net.status().requests.size() == 1 && net.status().requests[0].method == "GET",
          "status: the in-flight request");
    const auto before = std::chrono::steady_clock::now();
    net.stop();
    check(std::chrono::steady_clock::now() - before < 2s, "stop: returns promptly");
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (observed < 2 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    check(observed == 2, "stop: both servers see the close");
    check(net.status().connections.empty() && net.status().requests.empty() && net.poll().empty(),
          "stop: no connections, requests or queued events left");
    Server again([](Server& s) { s.accept(); });
    net.open(tcp("again", again.port()));
    check(ev.waitFor([&] { return ev.has("net.open", "again"); }), "a stopped Network opens again");
}

// --- kast.net (Lua) ---

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

const std::filesystem::path& netRoot() // removed by netSelfCheck
{
    static const std::filesystem::path root =
        std::filesystem::temp_directory_path() / std::format("kast_net_check_{}", _getpid());
    return root;
}

std::filesystem::path netProject(const char* name)
{
    const std::filesystem::path dir = netRoot() / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "scripts");
    writeFile(dir / kast::kProjectFileName, R"({"version": 1, "name": "Net", "startScene": null})");
    return dir;
}

void saveScene(const std::filesystem::path& file, std::vector<std::string> systems)
{
    kast::Scene scene;
    scene.create("E");
    scene.systems = std::move(systems);
    std::string error;
    check(kast::saveScene(scene, file.string(), error), "write a net test scene");
}

// Ticks Play until the log has part (or timeout).
bool playUntil(Document& doc, const std::string& part, std::chrono::milliseconds timeout = 5s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (!logged(part)) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        updatePlay(doc, 0.016f);
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

void checkNetLuaApi()
{
    const std::filesystem::path dir = netProject("kast_net_api_check");
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    writeFile(dir / "certs/ca.pem", cert.certPem);
    Server jsonEcho([](Server& s) {
        TlsSocket stream = s.acceptTls();
        std::string buffer;
        for (;;)
            asio::write(stream, asio::buffer(readLine(stream, buffer) + "\n"));
    }, 0, &cert);
    Server bytes([](Server& s) {
        websocket::stream<tcp::socket> stream(s.accept());
        stream.accept();
        for (;;) {
            beast::flat_buffer buffer;
            stream.read(buffer);
            stream.binary(true);
            stream.write(buffer.data());
        }
    });
    saveScene(dir / "api.scene", {"scripts/api.lua"});
    writeFile(dir / "scripts/api.lua", std::format(R"(local function fails(part, fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok and string.find(err, part, 1, true), "expected '" .. part .. "', got " .. tostring(err))
end
local got = {{}}
return {{
    init = function()
        fails("kast.net.open: expected an options table", kast.net.open, "x")
        fails("kast.net.open: type must be 'tcp' or 'ws'", kast.net.open, {{name = "a"}})
        fails("kast.net.open: unknown option bogus", kast.net.open, {{name = "a", type = "tcp", bogus = 1}})
        fails("kast.net.open: port must be an integer 1..65535", kast.net.open, {{name = "a", type = "tcp", host = "h", port = 1.5}})
        fails("kast.net.open: url doesn't apply to type 'tcp'", kast.net.open, {{name = "a", type = "tcp", url = "ws://h"}})
        fails("kast.net.open: framing doesn't apply to type 'ws'", kast.net.open, {{name = "a", type = "ws", url = "ws://h", framing = "line"}})
        fails("kast.net.open: encoding must be one of 'text', 'base64', 'json'", kast.net.open, {{name = "a", type = "tcp", host = "h", port = 1, encoding = "hex"}})
        fails("kast.net.open: timeout must be", kast.net.open, {{name = "a", type = "tcp", host = "h", port = 1, timeout = 0}})
        fails("kast.net.open: caFile 'certs/none.pem' can't be read", kast.net.open, {{name = "a", type = "tcp", host = "h", port = 1, tls = true, caFile = "certs/none.pem"}})
        fails("kast.net.open: url must start with ws:// or wss://", kast.net.open, {{name = "a", type = "ws", url = "http://h"}})
        fails("kast.net.request: url must start with http:// or https://", kast.net.request, {{url = "ws://h"}})
        fails("kast.net.request: headers must map strings to strings", kast.net.request, {{url = "http://h", headers = {{a = 1}}}})
        fails("kast.net.base64Decode: not valid base64", kast.net.base64Decode, "abc")
        assert(kast.net.base64Decode(kast.net.base64Encode("\0\255x")) == "\0\255x")
        assert(kast.net.get("j") == nil)
        local j = kast.net.open{{name = "j", type = "tcp", host = "localhost", port = {}, tls = true,
                                caFile = "certs/ca.pem", encoding = "json", framing = "line", timeout = 5}}
        assert(j.name == "j" and j:state() == "connecting" and kast.net.get("j").name == "j")
        fails("kast.net.open: connection 'j' is already open", kast.net.open, {{name = "j", type = "tcp", host = "h", port = 1}})
        fails("kast.net.send: ", j.send, j, function() end)
        j:send({{n = 1, list = {{1, 2}}}})
        local b = kast.net.open{{name = "b", type = "ws", url = "ws://127.0.0.1:{}/", encoding = "base64"}}
        fails("kast.net.send: data is not valid base64", b.send, b, "!!")
        b:send(kast.net.base64Encode("\1\2\3"))
        local t = kast.net.open{{name = "t", type = "ws", url = "ws://127.0.0.1:1/"}}
        fails("kast.net.send: data is not valid UTF-8", t.send, t, "\255")
        kast.net.close("t")
        assert(kast.net.get("t") == nil)
        kast.subscribe("net.message", function(e) got[e.conn] = e.data end)
    end,
    update = function(dt)
        if got.j and got.b then
            assert(got.j.n == 1 and got.j.list[2] == 2, "json round trip")
            assert(kast.net.base64Decode(got.b) == "\1\2\3", "base64 round trip")
            assert(kast.net.get("j"):state() == "open")
            kast.log("net api ok")
            got = {{}}
        end
    end,
}}
)",
                                                   jsonEcho.port(), bytes.port()));
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "api.scene").string(), error) && doc.project, "open the kast.net API scene");
    startPlay(doc);
    check(playUntil(doc, "net api ok"), "kast.net: argument errors, json and base64 sends round-trip");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        if (!s.ok)
            kast::log::error(std::format("  {}: {}", s.path, s.error));
    stopPlay(doc);
}

// Connections survive kast.loadScene with no event lost; Stop closes everything; Play starts with none.
void checkNetSceneSwitch()
{
    const std::filesystem::path dir = netProject("kast_net_switch_check");
    constexpr int kTicks = 500;
    std::atomic<int> closes = 0;
    Server ticker([&](Server& s) {
        tcp::socket socket = s.accept();
        std::string buffer;
        for (;;) {
            boost::system::error_code ec;
            const size_t n = asio::read_until(socket, asio::dynamic_buffer(buffer), '\n', ec);
            if (ec)
                break;
            const std::string line = buffer.substr(0, n - 1);
            buffer.erase(0, n);
            if (line == "stream") { // ticks before, during and after the switch
                for (int i = 1; i <= kTicks; ++i) {
                    asio::write(socket, asio::buffer(std::format("{}\n", i)));
                    if (i % 50 == 0)
                        std::this_thread::sleep_for(2ms);
                }
            } else {
                asio::write(socket, asio::buffer("echo " + line + "\n"));
            }
        }
        ++closes;
    });
    Server wsServer([&](Server& s) {
        websocket::stream<tcp::socket> stream(s.accept());
        stream.accept();
        boost::system::error_code ec;
        for (;;) {
            beast::flat_buffer buffer;
            stream.read(buffer, ec);
            if (ec)
                break;
            stream.write(buffer.data(), ec);
        }
        ++closes;
    });
    Server slowHttp([&](Server& s) {
        tcp::socket socket = s.accept();
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(socket, buffer, req);
        std::this_thread::sleep_for(200ms); // answered after the switch
        http::response<http::string_body> res{http::status::ok, 11};
        res.body() = "late";
        res.prepare_payload();
        http::write(socket, res);
    });
    saveScene(dir / "a.scene", {"scripts/a.lua"});
    saveScene(dir / "b.scene", {"scripts/b.lua"});
    writeFile(dir / "scripts/a.lua", std::format(R"(local opened = 0
return {{
    init = function()
        kast.net.open{{name = "ticks", type = "tcp", host = "127.0.0.1", port = {}, framing = "line"}}
        kast.net.open{{name = "chat", type = "ws", url = "ws://127.0.0.1:{}/"}}
        kast.subscribe("net.open", function(e)
            opened = opened + 1
            if opened == 2 then
                kast.net.get("ticks"):send("stream")
                kast.net.request{{url = "http://127.0.0.1:{}/slow"}}
                kast.log("net a switching")
                kast.loadScene("b.scene")
            end
        end)
    end,
    update = function(dt) end,
}}
)",
                                                 ticker.port(), wsServer.port(), slowHttp.port()));
    writeFile(dir / "scripts/b.lua", std::format(R"(local ticks, expected, chat, http = 0, 1, nil, nil
return {{
    init = function()
        local t, c = kast.net.get("ticks"), kast.net.get("chat")
        assert(t and c and t:state() == "open" and c:state() == "open", "connections survive the switch")
        c:send("after switch")
        kast.subscribe("net.message", function(e)
            if e.conn == "ticks" then
                assert(tonumber(e.data) == expected, "tick " .. e.data .. " out of order, expected " .. expected)
                expected = expected + 1
                ticks = ticks + 1
            else
                chat = e.data
            end
        end)
        kast.subscribe("net.http.response", function(e) http = e.body end)
    end,
    update = function(dt)
        if ticks == {} and chat and http then
            kast.log(string.format("net b got %d ticks, chat=%s, http=%s", ticks, chat, http))
            ticks = -1
        end
    end,
}}
)",
                                                 kTicks));
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open the net switch scene");
    startPlay(doc);
    const std::thread::id main = std::this_thread::get_id();
    bool offThread = false;
    kast::EventBus::Subscription sub = doc.lua.bus().subscribe("net.message", [&](const nlohmann::json&) {
        offThread |= std::this_thread::get_id() != main;
    });
    check(playUntil(doc, "net a switching"), "net: scene a opens both, then switches");
    check(playUntil(doc, std::format("net b got {} ticks, chat=after switch, http=late", kTicks)),
          "net: after kast.loadScene the new scene gets every tick in order, the ws echo and the earlier request's "
          "response");
    check(!offThread, "net: bus events are delivered on the main thread");
    check(ticker.accepted() == 1 && wsServer.accepted() == 1, "net: the switch reused the connections (one accept each)");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        if (!s.ok)
            kast::log::error(std::format("  {}: {}", s.path, s.error));
    check(doc.lua.network().status().connections.size() == 2, "net: the Network panel's status lists both");
    stopPlay(doc);
    const auto end = std::chrono::steady_clock::now() + 5s;
    while (closes < 2 && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(1ms);
    check(closes == 2, "net: Stop closes every connection (the servers see it)");
    check(doc.lua.network().status().connections.empty(), "net: no connections after Stop");
    startPlay(doc); // scene a again: opens new ones (the servers accept no more, so they stay connecting)
    check(doc.lua.network().status().connections.size() == 2 &&
              doc.lua.network().status().connections[0].bytesIn == 0,
          "net: Play again starts from zero connections");
    stopPlay(doc);
}

// --- kast::AuthClient and kast.auth, against FakeAuth (tests/net_test_util.h) ---

// An AuthClient on its own Network, pumped the way LuaSystems::tick does.
struct AuthRun {
    Network net;
    kast::AuthClient auth{net};
    std::vector<NetEvent> events;
    int maxRefreshes = 0;   // most refresh requests in flight at once
    bool unowned = false;   // a Network result the client didn't take
    std::optional<std::chrono::steady_clock::time_point> firstRefresh; // when one was first seen in flight

    explicit AuthRun(const std::string& url) { auth.reset(kast::AuthConfig{url, ""}); }
    int refreshesInFlight() const
    {
        return static_cast<int>(std::ranges::count_if(
            net.status().requests, [](const kast::NetRequestStatus& r) { return contains(r.url, "/v1/auth/refresh"); }));
    }
    void pump()
    {
        for (NetEvent& e : net.poll())
            unowned |= !auth.handle(e);
        for (NetEvent& e : auth.poll())
            events.push_back(std::move(e));
        const int refreshing = refreshesInFlight();
        maxRefreshes = std::max(maxRefreshes, refreshing);
        if (refreshing && !firstRefresh)
            firstRefresh = std::chrono::steady_clock::now();
    }
    bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            if (done())
                return true;
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(1ms);
        }
    }
    void pumpFor(std::chrono::milliseconds time)
    {
        waitFor([] { return false; }, time);
    }
    const Json* event(const std::string& name, uint64_t id = 0) const
    {
        for (const NetEvent& e : events)
            if (e.name == name && (!id || e.payload.value("id", uint64_t(0)) == id))
                return &e.payload;
        return nullptr;
    }
    // Waits for call id's result: its auth.error, or any other event carrying its id.
    const Json* result(uint64_t id)
    {
        const NetEvent* found = nullptr;
        waitFor([&] {
            for (const NetEvent& e : events)
                if (e.payload.is_object() && e.payload.value("id", uint64_t(0)) == id)
                    found = &e;
            return found != nullptr;
        });
        return found ? &found->payload : nullptr;
    }
    std::string nameOf(uint64_t id) const
    {
        for (const NetEvent& e : events)
            if (e.payload.is_object() && e.payload.value("id", uint64_t(0)) == id)
                return e.name;
        return "";
    }
    // call id ended in auth.error with code (and status, if given).
    bool failed(uint64_t id, const std::string& op, const std::string& code, int status = -1)
    {
        const Json* r = result(id);
        return r && nameOf(id) == "auth.error" && r->value("op", "") == op && r->value("code", "") == code &&
               (status < 0 || r->value("status", -1) == status) && r->contains("message");
    }
    bool succeeded(uint64_t id, const std::string& name) { return result(id) && nameOf(id) == name; }
};

void checkAuthClient()
{
    using std::chrono::seconds;
    check(kast::authRefreshDelay(seconds(900)) == 720s && kast::authRefreshDelay(seconds(120)) == 60s &&
              kast::authRefreshDelay(seconds(1)) == 500ms,
          "auth: refresh at 80% of the lifetime or 60 s before expiry, whichever is earlier, not before half");
    FakeAuth fake;
    Server server(serveFakeAuth(fake));
    AuthRun r(std::format("http://127.0.0.1:{}/", server.port())); // the trailing '/' is dropped
    auto& a = r.auth;
    check(a.enabled() && !a.loggedIn() && !a.accessToken() && a.account().is_null(), "auth: starts logged out");

    check(r.failed(a.me(), "me", "not_logged_in", 0), "auth: me while logged out -> auth.error not_logged_in");
    check(r.failed(a.realms(), "realms", "not_logged_in", 0), "auth: realms while logged out -> not_logged_in");
    check(r.failed(a.logout(), "logout", "not_logged_in", 0), "auth: logout while logged out -> not_logged_in");
    check(r.failed(a.login(kDemoEmail, "wrong-password"), "login", "invalid_credentials", 401),
          "auth: a wrong password -> 401 invalid_credentials");
    {
        const uint64_t id = a.login("locked@test", "whatever-password");
        check(r.failed(id, "login", "account_locked", 423) &&
                  r.result(id)->at("details").at("lockedUntil") == "2099-01-01T00:15:00.000Z",
              "auth: 423 account_locked carries details.lockedUntil");
    }
    {
        const uint64_t id = a.login("banned@test", "whatever-password");
        check(r.failed(id, "login", "account_banned", 403) && r.result(id)->at("details").at("reason") == "cheating",
              "auth: 403 account_banned carries the ban details");
    }
    {
        const uint64_t id = a.registerAccount("new@test", "short");
        check(r.failed(id, "register", "validation_error", 400) &&
                  r.result(id)->at("details").at(0).at("path") == "password",
              "auth: 400 validation_error carries the details array");
    }
    check(r.failed(a.verifyEmail("bad"), "verifyEmail", "invalid_token", 400), "auth: verifyEmail invalid_token");
    check(r.failed(a.resetPassword("bad", "long-enough-password"), "resetPassword", "invalid_token", 400),
          "auth: resetPassword invalid_token");
    check(r.succeeded(a.registerAccount("new@test", "long-enough-password"), "auth.registered"), "auth: register ok");
    check(r.succeeded(a.resendVerification("new@test"), "auth.verificationResent"), "auth: resendVerification ok");
    check(r.succeeded(a.verifyEmail("good"), "auth.emailVerified"), "auth: verifyEmail ok");
    check(r.succeeded(a.forgotPassword("new@test"), "auth.passwordResetSent"), "auth: forgotPassword ok");
    check(r.succeeded(a.resetPassword("good", "long-enough-password"), "auth.passwordReset"), "auth: resetPassword ok");

    const uint64_t login = a.login(kDemoEmail, kDemoPassword);
    check(r.failed(a.login(kDemoEmail, kDemoPassword), "login", "already_logged_in", 0),
          "auth: a second login while one is in flight -> already_logged_in");
    const Json* ok = r.result(login);
    check(ok && r.nameOf(login) == "auth.login.ok", "auth: login ok");
    if (ok)
        check(ok->value("accountId", 0) == 42 && ok->value("email", "") == kDemoEmail &&
                  ok->value("role", "") == "player" && !ok->contains("accessToken") && !ok->contains("refreshToken"),
              "auth.login.ok: {id, accountId, email, role} from the token's claims, no tokens");
    check(a.loggedIn() && a.accessToken() == fake.accessToken() && a.account().value("accountId", 0) == 42,
          "auth: the session holds the access token for C++ and the account");
    check(r.failed(a.login(kDemoEmail, kDemoPassword), "login", "already_logged_in", 0),
          "auth: login while logged in -> already_logged_in");
    {
        const uint64_t id = a.me();
        check(r.succeeded(id, "auth.me") && r.result(id)->at("account").at("email") == kDemoEmail,
              "auth.me: {id, account} (the server checked the Bearer token)");
    }
    {
        const uint64_t id = a.realms();
        const Json* realms = r.result(id);
        check(r.nameOf(id) == "auth.realms" && realms && realms->at("realms").size() == 3 &&
                  realms->at("realms").at(0).at("name") == "Ashenvale" &&
                  realms->at("realms").at(1).at("status") == "maintenance",
              "auth.realms: {id, realms:[{id, name, host, port, region, status, sortOrder}]}");
    }
    const uint64_t logout = a.logout();
    check(!a.loggedIn() && !a.accessToken(), "auth: logout clears the session at once");
    check(r.succeeded(logout, "auth.loggedOut"), "auth: logout -> auth.loggedOut");
    const std::vector<FakeAuth::Seen> seen = fake.seen();
    check(!seen.empty() && seen.back().path == "/v1/auth/logout" && seen.back().body.value("refreshToken", "") == "refresh-1",
          "auth: logout sends the session's refresh token");
    check(std::ranges::all_of(seen, [](const FakeAuth::Seen& s) { return s.path.starts_with("/v1/"); }),
          "auth: requests go to <authUrl>/v1/...");
    check(!r.unowned, "auth: every Network result of its requests went to the client");

    AuthRun down("http://127.0.0.1:1");
    check(down.failed(down.auth.registerAccount("a@test", "long-enough-password"), "register", "network_error", 0),
          "auth: an unreachable server -> auth.error network_error, status 0");
    AuthRun boom(std::format("http://127.0.0.1:{}/boom", server.port()));
    check(boom.failed(boom.auth.forgotPassword("a@test"), "forgotPassword", "http_error", 500),
          "auth: a non-JSON error response -> auth.error http_error with the status");
}

void checkAuthRefresh()
{
    FakeAuth fake;
    fake.expiresIn = 1; // refreshed after 0.5 s
    fake.refreshDelayMs = 300;
    Server server(serveFakeAuth(fake));
    AuthRun r(std::format("http://127.0.0.1:{}", server.port()));
    auto& a = r.auth;
    const uint64_t login = a.login(kDemoEmail, kDemoPassword);
    check(r.succeeded(login, "auth.login.ok"), "auth refresh: login");
    const auto loggedInAt = std::chrono::steady_clock::now();
    const std::string first = a.accessToken().value_or("");
    check(r.waitFor([&] { return r.refreshesInFlight() > 0; }), "auth refresh: a refresh starts");
    check(r.firstRefresh && *r.firstRefresh - loggedInAt < 1s, "auth refresh: it starts before the token expires");
    const uint64_t me = a.me(); // held until the refresh answers, then sent with the new token
    check(r.succeeded(me, "auth.me"), "auth refresh: me during a refresh waits for it and uses the new token");
    r.pumpFor(2000ms);
    check(fake.refreshes() >= 2, std::format("auth refresh: keeps refreshing ({} refreshes in 2.5 s)", fake.refreshes()));
    check(r.maxRefreshes == 1, std::format("auth refresh: at most one in flight (saw {})", r.maxRefreshes));
    check(a.loggedIn() && a.accessToken() != first && !r.event("auth.error") && !r.event("auth.session.expired"),
          "auth refresh: the session continues with new tokens, no errors");

    fake.refreshFails = true;
    check(r.waitFor([&] { return r.event("auth.session.expired") != nullptr; }),
          "auth refresh: a failed refresh publishes auth.session.expired");
    if (const Json* e = r.event("auth.session.expired"))
        check(e->value("code", "") == "invalid_refresh_token" && !a.loggedIn() && !a.accessToken(),
              "auth.session.expired: {code, message}; the session is cleared");
    check(!r.event("auth.error"), "auth refresh: an expired session is not also an auth.error");

    // A refresh still in flight at logout is ignored when it answers.
    fake.refreshFails = false;
    r.events.clear();
    check(r.succeeded(a.login(kDemoEmail, kDemoPassword), "auth.login.ok"), "auth refresh: log in again");
    check(r.waitFor([&] { return r.refreshesInFlight() > 0; }), "auth refresh: a refresh starts again");
    check(r.succeeded(a.logout(), "auth.loggedOut"), "auth refresh: logout during a refresh");
    r.pumpFor(600ms);
    check(!a.loggedIn() && r.events.size() == 2, "auth refresh: the late refresh answer doesn't log back in");

    kast::AuthSession adopted{.accessToken = "a", .refreshToken = "r", .expiresIn = 900s,
                              .account = {{"accountId", 7}, {"email", "editor@test"}, {"role", "gm"}}};
    a.adopt(adopted);
    r.pump();
    const Json* session = r.event("auth.session");
    check(a.loggedIn() && a.accessToken() == "a" && session && session->value("accountId", 0) == 7,
          "auth: adopt starts that session and queues auth.session {accountId, email, role}");
    a.reset();
    check(!a.enabled() && !a.loggedIn(), "auth: reset() disables and logs out");
}

// kast.auth in Play: nil while Online Services are off; with them on, every function, events from "kast.auth", the
// session kept across kast.loadScene, no auth bodies on the bus as net.* events, logged out by Stop.
void checkAuthLuaApi()
{
    const std::filesystem::path off = netProject("kast_auth_off_check");
    saveScene(off / "off.scene", {"scripts/off.lua"});
    writeFile(off / "scripts/off.lua", R"(return { init = function() kast.log("auth off: " .. tostring(kast.auth) .. " world off: " .. tostring(kast.world)) end, update = function(dt) end })");
    {
        Document doc;
        std::string error;
        check(openFile(doc, (off / "off.scene").string(), error), "open the auth-off scene");
        startPlay(doc);
        check(playUntil(doc, "auth off: nil world off: nil"), "kast.auth and kast.world are nil while Online Services are disabled");
        stopPlay(doc);
    }

    FakeAuth fake;
    Server server(serveFakeAuth(fake));
    const std::filesystem::path dir = netProject("kast_auth_api_check");
    writeFile(dir / kast::kProjectFileName,
              std::format(R"({{"version": 1, "name": "Auth", "startScene": null,
"online": {{"enabled": true, "authUrl": "http://127.0.0.1:{}"}}}})",
                          server.port()));
    saveScene(dir / "a.scene", {"scripts/a.lua"});
    saveScene(dir / "b.scene", {"scripts/b.lua"});
    writeFile(dir / "scripts/a.lua", std::format(R"(local function fails(part, fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok and string.find(err, part, 1, true), "expected '" .. part .. "', got " .. tostring(err))
end
return {{
    init = function()
        for _, name in ipairs({{"register", "resendVerification", "verifyEmail", "login", "logout", "forgotPassword",
                               "resetPassword", "me", "realms", "isLoggedIn", "account"}}) do
            assert(type(kast.auth[name]) == "function", name .. " missing")
        end
        assert(kast.auth.accessToken == nil, "no raw tokens in Lua")
        fails("kast.auth.login: email must be a string, got 1", kast.auth.login, 1, "x")
        fails("kast.auth.register: password must be a string, got nil", kast.auth.register, "a@b")
        fails("kast.auth.verifyEmail: token must be a string", kast.auth.verifyEmail, {{}})
        assert(kast.auth.isLoggedIn() == false and kast.auth.account() == nil)
        kast.subscribe("auth.login.ok", function(e)
            assert(kast.auth.isLoggedIn() and kast.auth.account().accountId == 42)
            kast.log("lua auth login " .. e.accountId .. " " .. e.email .. " " .. e.role)
            kast.loadScene("b.scene")
        end)
        local id = kast.auth.login("{}", "{}")
        assert(type(id) == "number" and id >= 1)
    end,
    update = function(dt) end,
}}
)",
                                                 kDemoEmail, kDemoPassword));
    writeFile(dir / "scripts/b.lua", R"(return {
    init = function()
        assert(kast.auth.isLoggedIn(), "the session survives kast.loadScene")
        kast.subscribe("auth.realms", function(e) kast.log("lua auth realms " .. #e.realms .. " " .. e.realms[1].name) end)
        kast.subscribe("auth.me", function(e) kast.log("lua auth me " .. e.account.email) end)
        kast.subscribe("auth.error", function(e)
            kast.log(string.format("lua auth error %s %s %d %s", e.op, e.code, e.status, e.details[1].path))
        end)
        kast.subscribe("auth.loggedOut", function(e)
            kast.log("lua auth logged out " .. tostring(kast.auth.isLoggedIn()))
        end)
        kast.auth.realms()
        kast.auth.me()
        kast.auth.register("x@test", "short")
    end,
    update = function(dt) end,
}
)");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open the kast.auth scene");
    int netResponses = 0;
    startPlay(doc);
    kast::EventBus::Subscription sub =
        doc.lua.bus().subscribe("net.http.response", [&](const nlohmann::json&) { ++netResponses; });
    check(playUntil(doc, std::format("lua auth login 42 {} player", kDemoEmail)), "kast.auth.login -> auth.login.ok");
    check(playUntil(doc, "lua auth realms 3 Ashenvale"), "kast.auth.realms -> auth.realms (after a scene switch)");
    check(playUntil(doc, std::format("lua auth me {}", kDemoEmail)), "kast.auth.me -> auth.me");
    check(playUntil(doc, "lua auth error register validation_error 400 password"),
          "kast.auth.register -> auth.error {op, code, status, details}");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        if (!s.ok)
            kast::log::error(std::format("  {}: {}", s.path, s.error));
    check(netResponses == 0, "kast.auth's responses are not published as net.http.response");
    bool fromAuth = false;
    for (const kast::EventBus::Delivered& d : doc.lua.bus().history())
        fromAuth |= d.name == "auth.login.ok" && d.publisher == "kast.auth";
    check(fromAuth, "auth.* events are published by \"kast.auth\"");
    check(doc.lua.auth().loggedIn() && doc.lua.auth().accessToken() == fake.accessToken(),
          "C++ reads the Play session's access token through LuaSystems::auth()");
    std::string history;
    for (const kast::EventBus::Delivered& d : doc.lua.bus().history())
        history += d.payload;
    check(!contains(history, fake.accessToken()) && !contains(history, "refresh-") && !contains(history, kDemoPassword),
          "no event payload holds a token or the password");
    stopPlay(doc);
    check(!doc.lua.auth().loggedIn() && !doc.lua.auth().enabled(), "Stop logs out (the session is memory only)");
    startPlay(doc);
    check(doc.lua.auth().enabled() && !doc.lua.auth().loggedIn(), "Play again starts logged out");
    stopPlay(doc);
}

// The checks above logged in, refreshed and failed with the demo password and many tokens; none reached the log.
void checkAuthLogs()
{
    const bool leaked = std::ranges::any_of(kast::log::lines(), [](const std::string& line) {
        // every fake access token starts with base64url({"alg":"none" ...); refresh tokens are "refresh-<n>"
        return contains(line, kDemoPassword) || contains(line, "long-enough-password") ||
               contains(line, "eyJhbGciOiJub25lIi") || contains(line, "refresh-");
    });
    check(!leaked, "auth: no password or token in the log");
}

// --- kast::WorldClient and kast.world, against FakeAuth + FakeWorld (tests/net_test_util.h) ---

// A WorldClient with its AuthClient on their own Network, pumped the way LuaSystems::tick does.
struct WorldRun {
    Network net;
    kast::AuthClient auth{net};
    kast::WorldClient world{net, auth};
    std::vector<NetEvent> events;     // world.*
    std::vector<NetEvent> authEvents; // auth.*
    std::unordered_map<std::string, size_t> taken; // per event name: how many next() returned
    bool unowned = false;

    WorldRun(const std::string& authUrl, const std::string& caPem)
    {
        auth.reset(kast::AuthConfig{authUrl, caPem});
        world.reset(kast::AuthConfig{authUrl, caPem});
    }
    void pump()
    {
        for (NetEvent& e : net.poll())
            unowned |= !auth.handle(e) && !world.handle(e);
        for (NetEvent& e : auth.poll()) {
            world.observe(e);
            authEvents.push_back(std::move(e));
        }
        for (NetEvent& e : world.poll())
            events.push_back(std::move(e));
    }
    bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds timeout = 5s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            pump();
            if (done())
                return true;
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(1ms);
        }
    }
    // The next event called name not returned before (waits for it); nullptr on timeout.
    const Json* next(const std::string& name, std::chrono::milliseconds timeout = 5s)
    {
        const Json* found = nullptr;
        waitFor([&] {
            size_t seen = 0;
            for (const NetEvent& e : events)
                if (e.name == name && seen++ == taken[name]) {
                    found = &e.payload;
                    return true;
                }
            return false;
        }, timeout);
        if (found)
            ++taken[name];
        return found;
    }
    size_t count(const std::string& name) const
    {
        return static_cast<size_t>(std::ranges::count_if(events, [&](const NetEvent& e) { return e.name == name; }));
    }
    bool login()
    {
        auth.login(kDemoEmail, kDemoPassword);
        return waitFor([&] { return auth.loggedIn(); });
    }
    // connect(realm) answered by world.connected
    bool connect(uint64_t realm)
    {
        world.connect(realm);
        const Json* e = next("world.connected");
        return e && world.state() == kast::WorldState::Lobby;
    }
};

bool throwsLogic(const std::function<void()>& fn, const std::string& part)
{
    try {
        fn();
    } catch (const std::logic_error& e) {
        return contains(e.what(), part);
    }
    return false;
}

Json pos(float x, float y, float z) { return {{"x", x}, {"y", y}, {"z", z}}; }

void checkWorldClient()
{
    check(kast::parseHostPort("127.0.0.1:7777") == std::pair(std::string("127.0.0.1"), uint16_t{7777}) &&
              kast::parseHostPort("world.example.com:1") && !kast::parseHostPort("host") &&
              !kast::parseHostPort(":7777") && !kast::parseHostPort("host:") && !kast::parseHostPort("host:0") &&
              !kast::parseHostPort("host:65536") && !kast::parseHostPort("host:77x"),
          "world: parseHostPort accepts host:port only");
    {
        Network net;
        kast::AuthClient auth{net};
        kast::WorldClient world{net, auth};
        check(!world.enabled() && world.state() == kast::WorldState::Disconnected &&
                  throwsLogic([&] { world.connect(1); }, "connect: Online Services are disabled"),
              "world: disabled until reset with a config; calls throw");
        check(throwsInvalid([&] { world.setAddressOverride("no-port"); }, "is not host:port") &&
                  !world.addressOverride(),
              "world: a bad address override is refused");
    }

    FakeAuth fake;
    Server authServer(serveFakeAuth(fake));
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    FakeWorld fakeWorld;
    Server worldServer(serveFakeWorld(fakeWorld), 0, &cert);
    fake.worldPort = worldServer.port();
    WorldRun r(std::format("http://127.0.0.1:{}", authServer.port()), cert.certPem);
    auto& w = r.world;
    check(w.enabled() && kast::worldStateName(w.state()) == std::string("disconnected"), "world: starts disconnected");

    w.connect(3);
    const Json* noSession = r.next("world.disconnected");
    check(noSession && noSession->value("code", "") == "not_logged_in" && w.state() == kast::WorldState::Disconnected,
          "world: connect without an auth session -> world.disconnected not_logged_in");
    check(r.net.status().connections.empty() && r.net.status().requests.empty() && fake.seen().empty(),
          "world: ... and no network attempt");

    check(r.login(), "world: log in to the fake auth");
    w.connect(99);
    check(w.state() == kast::WorldState::Connecting, "world: connecting while the realm list is fetched");
    const Json* unknown = r.next("world.disconnected");
    check(unknown && unknown->value("code", "") == "unknown_realm" && contains(unknown->value("reason", ""), "99"),
          "world: an unknown realm id -> world.disconnected unknown_realm");
    check(fake.seen().back().path == "/v1/realms", "world: connect fetched the realm list");
    const size_t requestsBefore = fake.seen().size();

    w.connect(3); // Testgrove, from the cached list
    const Json* connected = r.next("world.connected");
    check(connected && connected->value("accountId", 0) == 42 && connected->value("role", "") == "player" &&
              w.state() == kast::WorldState::Lobby,
          "world: Hello + Auth -> world.connected {accountId, role}; state lobby");
    check(fake.seen().size() == requestsBefore, "world: a cached realm needs no new realm request");
    check(r.waitFor([&] { return fakeWorld.pongs() == 1; }), "world: the server's Ping is answered with its nonce");
    {
        const std::vector<proto::Message> seen = fakeWorld.seen();
        check(seen.size() >= 2 && std::get_if<proto::Hello>(&seen[0]) &&
                  std::get_if<proto::Hello>(&seen[0])->protocolVersion == 1 && std::get_if<proto::Auth>(&seen[1]) &&
                  std::get_if<proto::Auth>(&seen[1])->accessToken == fake.accessToken(),
              "world: the handshake sends Hello{1} then Auth{the session's access token}");
    }
    check(throwsLogic([&] { w.connect(3); }, "connect: needs state 'disconnected', the connection is 'lobby'") &&
              throwsLogic([&] { w.say("x"); }, "say: needs state 'inWorld'"),
          "world: connect while connected and say in the lobby throw");

    w.listCharacters();
    const Json* empty = r.next("world.charList");
    check(empty && empty->at("characters").empty(), "world.charList {characters = {}}");
    w.createCharacter("Taken");
    const Json* taken = r.next("world.charError");
    check(taken && taken->value("request", "") == "CharCreate" && taken->value("code", "") == "name_taken" &&
              w.state() == kast::WorldState::Lobby,
          "world.charError {request, code}; the connection stays");
    w.createCharacter("Hero");
    const Json* hero = r.next("world.charCreated");
    check(hero && hero->at("character") == Json{{"id", 100}, {"name", "Hero"}, {"position", pos(100, 0, 100)}},
          "world.charCreated {character = {id, name, position}}");
    w.createCharacter("Split");
    const Json* split = r.next("world.charCreated");
    check(split && split->at("character").value("id", 0) == 101, "world: a frame split across reads is reassembled");
    w.deleteCharacter(101);
    const Json* deleted = r.next("world.charDeleted");
    check(deleted && deleted->value("charId", 0) == 101, "world.charDeleted {charId}");
    w.deleteCharacter(999);
    const Json* notFound = r.next("world.charError");
    check(notFound && notFound->value("request", "") == "CharDelete" && notFound->value("code", "") == "not_found",
          "world: deleting an unknown character -> world.charError not_found");
    w.listCharacters();
    const Json* one = r.next("world.charList");
    check(one && one->at("characters").size() == 1 && one->at("characters")[0].value("name", "") == "Hero",
          "world.charList lists the characters");

    w.enterWorld(100);
    const Json* entered = r.next("world.entered");
    check(entered && entered->value("entityId", 0) == 1 && entered->at("position") == pos(100, 0, 100) &&
              entered->value("heading", 0.0) == 0.5 && w.state() == kast::WorldState::InWorld,
          "world.entered {entityId, position, heading}; state inWorld");
    const Json* spawn = r.next("world.spawn");
    check(spawn && *spawn == Json{{"entityId", 2}, {"name", "Other"}, {"position", pos(101, 0, 100)}, {"heading", 0.0}},
          "world.spawn {entityId, name, position, heading}");
    const Json* moved = r.next("world.entityMove");
    check(moved && *moved == Json{{"entityId", 2}, {"position", pos(102, 0, 100)}, {"heading", 1.0}},
          "world.entityMove {entityId, position, heading}");
    const Json* correction = r.next("world.correction");
    check(correction && *correction == Json{{"position", pos(100, 0, 100)}}, "world.correction {position}");
    check(throwsLogic([&] { w.listCharacters(); }, "listCharacters: needs state 'lobby'"),
          "world: character calls in the world throw");
    w.say("secret-chat-text-1");
    const Json* chat = r.next("world.chat");
    check(chat && *chat == Json{{"from", "Hero"}, {"text", "secret-chat-text-1"}}, "world.chat {from, text}");
    w.say("bye");
    const Json* despawn = r.next("world.despawn");
    check(despawn && *despawn == Json{{"entityId", 2}}, "world.despawn {entityId}");

    w.disconnect();
    const Json* bye = r.next("world.disconnected");
    check(bye && bye->value("code", "") == "client_disconnect" && w.state() == kast::WorldState::Disconnected,
          "world: disconnect -> world.disconnected client_disconnect");
    w.disconnect();
    r.pump();
    check(r.count("world.disconnected") == 3, "world: disconnect while disconnected publishes nothing");

    // Every code the server can send maps to its PROTOCOL.md name; an unknown value to "unknown".
    for (const int code : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20, 21, 22, 23, 24, 99}) {
        const char* name = code == 99 ? "unknown" : proto::code_name(static_cast<proto::Code>(code));
        if (!r.connect(3)) {
            check(false, std::format("world: connect for Disconnect code {}", code));
            break;
        }
        w.createCharacter(std::format("Kick{}", code));
        const Json* kicked = r.next("world.disconnected");
        check(kicked && kicked->value("code", "") == name &&
                  contains(kicked->value("reason", ""), std::format("code {}", code)) &&
                  w.state() == kast::WorldState::Disconnected,
              std::format("world: Disconnect{{{}}} -> world.disconnected {{code = {}}}", code, name));
    }
    r.pump();
    check(r.count("world.disconnected") == 24, "world: one world.disconnected per Disconnect (not another for the close)");

    check(r.connect(3), "world: connect for a malformed frame");
    w.createCharacter("Garbage");
    const Json* garbage = r.next("world.disconnected");
    check(garbage && garbage->value("code", "") == "malformed" && contains(garbage->value("reason", ""), "Spawn"),
          "world: a malformed frame -> world.disconnected malformed, closed");
    check(r.connect(3), "world: connect for an unknown opcode");
    w.createCharacter("Unknown");
    const Json* unknownOp = r.next("world.disconnected");
    check(unknownOp && unknownOp->value("code", "") == "unknown_opcode" &&
              contains(unknownOp->value("reason", ""), "0x7777"),
          "world: an unknown opcode -> world.disconnected unknown_opcode, closed");

    w.connect(1); // Ashenvale: 127.0.0.1:4000, nothing listening
    const Json* refused = r.next("world.disconnected", 15s);
    check(refused && refused->value("code", "") == "connect_failed", "world: an unreachable realm -> connect_failed");
    w.setAddressOverride(std::format("127.0.0.1:{}", worldServer.port()));
    check(r.connect(1), "world: the address override replaces the realm's host:port");
    w.setAddressOverride(std::nullopt);
    w.disconnect();
    r.next("world.disconnected");
    check(!r.unowned, "world: every Network result of its connections went to the client");
    check(std::ranges::none_of(kast::log::lines(),
                               [&](const std::string& line) {
                                   return contains(line, "secret-chat-text") || contains(line, fake.accessToken());
                               }),
          "world: no chat text or access token in the log");
    w.reset();
    check(!w.enabled(), "world: reset() disables");
}

// kast.world in Play: nil while Online Services are off (checkAuthLuaApi), every function and its errors, the whole
// flow from Lua, the connection kept across kast.loadScene, events from "kast.world", no raw frames on the bus.
void checkWorldLuaApi()
{
    FakeAuth fake;
    Server authServer(serveFakeAuth(fake));
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    FakeWorld fakeWorld;
    Server worldServer(serveFakeWorld(fakeWorld), 0, &cert);
    fake.worldPort = worldServer.port();
    const std::filesystem::path dir = netProject("kast_world_api_check");
    writeFile(dir / "certs/world_ca.pem", cert.certPem);
    writeFile(dir / kast::kProjectFileName,
              std::format(R"({{"version": 1, "name": "World", "startScene": null,
"online": {{"enabled": true, "authUrl": "http://127.0.0.1:{}", "caFile": "certs/world_ca.pem"}}}})",
                          authServer.port()));
    saveScene(dir / "a.scene", {"scripts/a.lua"});
    saveScene(dir / "b.scene", {"scripts/b.lua"});
    writeFile(dir / "scripts/a.lua", std::format(R"(local function fails(part, fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok and string.find(err, part, 1, true), "expected '" .. part .. "', got " .. tostring(err))
end
return {{
    init = function()
        for _, name in ipairs({{"connect", "disconnect", "listCharacters", "createCharacter", "deleteCharacter",
                               "enterWorld", "say", "state"}}) do
            assert(type(kast.world[name]) == "function", name .. " missing")
        end
        assert(kast.world.state() == "disconnected")
        fails("kast.world.connect: realmId must be a whole number >= 0, got x", kast.world.connect, "x")
        fails("kast.world.connect: realmId must be a whole number >= 0, got 1.5", kast.world.connect, 1.5)
        fails("kast.world.createCharacter: name must be a string, got nil", kast.world.createCharacter)
        fails("kast.world.say: needs state 'inWorld', the connection is 'disconnected'", kast.world.say, "hi")
        fails("kast.world.listCharacters: needs state 'lobby'", kast.world.listCharacters)
        kast.subscribe("world.disconnected", function(e) kast.log("lua world disconnected " .. e.code) end)
        kast.subscribe("world.connected", function(e)
            kast.log(string.format("lua world connected %d %s %s", e.accountId, e.role, kast.world.state()))
            kast.world.createCharacter("Hero")
        end)
        kast.subscribe("world.charCreated", function(e)
            kast.log("lua world created " .. e.character.name .. " " .. e.character.id)
            kast.world.enterWorld(e.character.id)
        end)
        kast.subscribe("world.entered", function(e)
            kast.log(string.format("lua world entered %d at %g,%g,%g", e.entityId, e.position.x, e.position.y,
                                   e.position.z))
            kast.loadScene("b.scene")
        end)
        kast.subscribe("auth.login.ok", function() kast.world.connect(3) end)
        kast.world.connect(3) -- no session yet
        kast.auth.login("{}", "{}")
    end,
    update = function(dt) end,
}}
)",
                                                 kDemoEmail, kDemoPassword));
    writeFile(dir / "scripts/b.lua", R"(return {
    init = function()
        assert(kast.world.state() == "inWorld", "the connection survives kast.loadScene")
        kast.subscribe("world.spawn", function(e) kast.log("lua world spawn " .. e.entityId .. " " .. e.name) end)
        kast.subscribe("world.chat", function(e) kast.log("lua world chat from " .. e.from) end)
        kast.subscribe("world.despawn", function(e)
            kast.log("lua world despawn " .. e.entityId)
            kast.world.disconnect()
        end)
        kast.subscribe("world.disconnected", function(e)
            kast.log("lua world b disconnected " .. e.code .. " " .. kast.world.state())
        end)
        kast.world.say("bye")
    end,
    update = function(dt) end,
}
)");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open the kast.world scene");
    int netMessages = 0;
    startPlay(doc);
    kast::EventBus::Subscription sub =
        doc.lua.bus().subscribe("net.message", [&](const nlohmann::json&) { ++netMessages; });
    check(playUntil(doc, "lua world disconnected not_logged_in"), "kast.world.connect before login -> not_logged_in");
    check(playUntil(doc, "lua world connected 42 player lobby"), "kast.world.connect -> world.connected, state lobby");
    check(playUntil(doc, "lua world created Hero 100"), "kast.world.createCharacter -> world.charCreated");
    check(playUntil(doc, "lua world entered 1 at 100,0,100"), "kast.world.enterWorld -> world.entered");
    check(playUntil(doc, "lua world chat from Hero"), "kast.world.say -> world.chat (after a scene switch)");
    check(playUntil(doc, "lua world despawn 2"), "world.despawn");
    check(playUntil(doc, "lua world b disconnected client_disconnect disconnected"),
          "kast.world.disconnect -> world.disconnected client_disconnect");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        if (!s.ok)
            kast::log::error(std::format("  {}: {}", s.path, s.error));
    check(netMessages == 0, "kast.world's frames are not published as net.message");
    bool fromWorld = false;
    std::string history;
    for (const kast::EventBus::Delivered& d : doc.lua.bus().history()) {
        fromWorld |= d.name == "world.connected" && d.publisher == "kast.world";
        history += d.payload;
    }
    check(fromWorld, "world.* events are published by \"kast.world\"");
    check(!contains(history, fake.accessToken()), "no event payload holds the access token");
    stopPlay(doc);
    check(!doc.lua.world().enabled() && doc.lua.world().state() == kast::WorldState::Disconnected,
          "Stop drops the world connection");
}

// --- live: kast::AuthClient against the real services/auth (the services/world compose stack) ---

// One GET on net, waited for; the response body as text, or nullopt on any failure.
std::optional<std::string> fetch(Network& net, const std::string& url)
{
    Events ev{net};
    const uint64_t id = net.request({.url = url, .timeout = 3s});
    if (!ev.waitFor([&] { return ev.response(id) || ev.response(id, "net.http.error"); }, 5s) || !ev.response(id) ||
        ev.response(id)->value("status", 0) != 200)
        return std::nullopt;
    return ev.response(id)->at("body").get<std::string>();
}

} // namespace

// kast_self_check --live-auth <authUrl> <mailpitUrl>: registers a fresh account, reads the verification token from
// Mailpit, verifies, logs in, reads me and realms and logs out. 77 (skipped) when the stack isn't up or registration
// is rate limited (5/hour/IP); 0 pass, 1 fail.
int liveAuthCheck(const std::string& authUrl, const std::string& mailpitUrl)
{
    constexpr int kSkipped = 77;
    Network probe;
    if (!fetch(probe, authUrl + "/health") || !fetch(probe, mailpitUrl + "/api/v1/info")) {
        kast::log::warn(std::format("SKIPPED: auth ({}) or Mailpit ({}) is not running; start it with "
                                    "'cd services/world && docker compose up -d --build'",
                                    authUrl, mailpitUrl));
        return kSkipped;
    }
    const std::string email = std::format("kast-live-{}@example.com",
                                          std::chrono::system_clock::now().time_since_epoch().count());
    const std::string password = "live-check-password-1";
    AuthRun r(authUrl);
    auto& a = r.auth;
    const uint64_t reg = a.registerAccount(email, password);
    if (r.failed(reg, "register", "rate_limited")) {
        kast::log::warn("SKIPPED: registration is rate limited (5/hour/IP); try again later");
        return kSkipped;
    }
    check(r.nameOf(reg) == "auth.registered", "live: register");
    std::string token;
    const auto end = std::chrono::steady_clock::now() + 15s;
    while (token.empty() && std::chrono::steady_clock::now() < end) {
        const std::optional<std::string> found = fetch(probe, std::format("{}/api/v1/search?query=to:{}", mailpitUrl, email));
        const Json list = found ? Json::parse(*found, nullptr, false) : Json();
        if (list.is_object() && list.contains("messages") && !list["messages"].empty()) {
            const std::string id = list["messages"][0].value("ID", "");
            const std::optional<std::string> message = fetch(probe, std::format("{}/api/v1/message/{}", mailpitUrl, id));
            const Json body = message ? Json::parse(*message, nullptr, false) : Json();
            const std::string text = body.is_object() ? body.value("Text", "") : "";
            const std::string marker = "Verification token: ";
            if (const size_t at = text.find(marker); at != std::string::npos)
                token = text.substr(at + marker.size(),
                                    text.find_first_of("\r\n", at + marker.size()) - at - marker.size());
        }
        if (token.empty())
            std::this_thread::sleep_for(250ms);
    }
    check(!token.empty(), "live: the verification email reaches Mailpit");
    check(r.succeeded(a.verifyEmail(token), "auth.emailVerified"), "live: verifyEmail");
    const uint64_t login = a.login(email, password);
    check(r.succeeded(login, "auth.login.ok") && r.result(login)->value("email", "") == email, "live: login");
    const std::string access = a.accessToken().value_or("no token");
    const uint64_t me = a.me();
    check(r.succeeded(me, "auth.me") && r.result(me)->at("account").value("emailVerified", false),
          "live: me (verified)");
    const uint64_t realms = a.realms();
    check(r.succeeded(realms, "auth.realms") && r.result(realms)->at("realms").is_array(), "live: realms");
    if (r.nameOf(realms) == "auth.realms")
        kast::log::info(std::format("live: {} realm(s)", r.result(realms)->at("realms").size()));
    check(r.succeeded(a.logout(), "auth.loggedOut") && !a.loggedIn(), "live: logout");
    for (const NetEvent& e : r.events)
        if (e.name == "auth.error")
            kast::log::error(std::format("live: auth.error {}", e.payload.dump()));
    check(std::ranges::none_of(kast::log::lines(),
                               [&](const std::string& line) {
                                   return contains(line, password) || (!token.empty() && contains(line, token)) || contains(line, access);
                               }),
          "live: no password or token in the log");
    if (g_failures == 0)
        kast::log::info("live auth check passed");
    return g_failures == 0 ? 0 : 1;
}

namespace {

// The verification token from the newest Mailpit message to email ("" if none arrives within 15 s).
std::string mailToken(Network& probe, const std::string& mailpitUrl, const std::string& email)
{
    const auto end = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < end) {
        const std::optional<std::string> found = fetch(probe, std::format("{}/api/v1/search?query=to:{}", mailpitUrl, email));
        const Json list = found ? Json::parse(*found, nullptr, false) : Json();
        if (list.is_object() && list.contains("messages") && !list["messages"].empty()) {
            const std::string id = list["messages"][0].value("ID", "");
            const std::optional<std::string> message = fetch(probe, std::format("{}/api/v1/message/{}", mailpitUrl, id));
            const Json body = message ? Json::parse(*message, nullptr, false) : Json();
            const std::string text = body.is_object() ? body.value("Text", "") : "";
            const std::string marker = "Verification token: ";
            if (const size_t at = text.find(marker); at != std::string::npos)
                return text.substr(at + marker.size(),
                                   text.find_first_of("\r\n", at + marker.size()) - at - marker.size());
        }
        std::this_thread::sleep_for(250ms);
    }
    return "";
}

// One JSON POST on net with X-Forwarded-For (the test stack's auth trusts it, so its per-IP limits apply per account);
// {status, body} or nullopt if the request failed.
std::optional<std::pair<int, Json>> post(Network& net, const std::string& url, const Json& body, const std::string& ip)
{
    Events ev{net};
    const uint64_t id = net.request({.method = "POST",
                                     .url = url,
                                     .headers = {{"Content-Type", "application/json"}, {"X-Forwarded-For", ip}},
                                     .body = body.dump(),
                                     .timeout = 5s});
    if (!ev.waitFor([&] { return ev.response(id) || ev.response(id, "net.http.error"); }, 10s) || !ev.response(id))
        return std::nullopt;
    const Json* r = ev.response(id);
    return std::pair(r->value("status", 0), Json::parse(r->value("body", ""), nullptr, false));
}

// 3-16 ASCII letters, new every run (world names stay reserved after delete).
std::string uniqueName(const char* prefix)
{
    static uint64_t bump = 0;
    uint64_t n = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) + ++bump;
    std::string name = prefix;
    while (name.size() < 14) {
        name += static_cast<char>('a' + n % 26);
        n /= 26;
    }
    return name;
}

// A verified, logged-in account adopted into r.auth; false if any step failed (logged).
bool liveAccount(WorldRun& r, Network& probe, const std::string& authUrl, const std::string& mailpitUrl,
                 const std::string& email, const std::string& password, const std::string& ip)
{
    const auto reg = post(probe, authUrl + "/v1/auth/register", {{"email", email}, {"password", password}}, ip);
    if (!reg || reg->first != 202) {
        kast::log::error(std::format("live world: register {} -> {}", email, reg ? reg->first : 0));
        return false;
    }
    const std::string token = mailToken(probe, mailpitUrl, email);
    const auto verified = post(probe, authUrl + "/v1/auth/verify-email", {{"token", token}}, ip);
    const auto login = post(probe, authUrl + "/v1/auth/login", {{"email", email}, {"password", password}}, ip);
    if (token.empty() || !verified || verified->first != 200 || !login || login->first != 200 ||
        !login->second.contains("accessToken")) {
        kast::log::error(std::format("live world: verify / login of {} failed", email));
        return false;
    }
    r.auth.adopt({.accessToken = login->second["accessToken"],
                  .refreshToken = login->second["refreshToken"],
                  .expiresIn = std::chrono::seconds(login->second.value("expiresIn", 900)),
                  .account = {{"email", email}}});
    return true;
}

} // namespace

// kast_self_check --live-world <authUrl> <mailpitUrl> <caFile> <realmName>: two fresh accounts on the services/world
// compose stack, through the realm called realmName (which must point at the world server). A: lobby characters
// (create, duplicate, delete, list), idles in the lobby past IDLE_TIMEOUT_MS + a heartbeat (Ping answered), enters the world. B enters
// next to it: A gets world.spawn, B's chat reaches A as world.chat, B's disconnect is world.despawn on A. 77 (skipped)
// when the stack isn't up or caFile is missing; 0 pass, 1 fail.
int liveWorldCheck(const std::string& authUrl, const std::string& mailpitUrl, const std::string& caFile,
                   const std::string& realmName)
{
    constexpr int kSkipped = 77;
    Network probe;
    std::ifstream caIn(caFile, std::ios::binary);
    const std::string caPem((std::istreambuf_iterator<char>(caIn)), std::istreambuf_iterator<char>());
    if (!fetch(probe, authUrl + "/health") || !fetch(probe, mailpitUrl + "/api/v1/info") || caPem.empty()) {
        kast::log::warn(std::format("SKIPPED: auth ({}), Mailpit ({}) or the world CA ({}) is missing; start the stack "
                                    "with 'cd services/world && docker compose up -d --build' and re-run ctest",
                                    authUrl, mailpitUrl, caFile));
        return kSkipped;
    }
    const std::string password = "live-world-password-1";
    const std::string stamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    WorldRun a(authUrl, caPem), b(authUrl, caPem);
    const uint64_t n = std::stoull(stamp) / 1000; // a new X-Forwarded-For per run and account
    const auto ip = [&](int account) { return std::format("10.{}.{}.{}", 70 + account, (n >> 8) % 256, n % 256); };
    if (!liveAccount(a, probe, authUrl, mailpitUrl, std::format("kast-world-a-{}@example.com", stamp), password, ip(1)) ||
        !liveAccount(b, probe, authUrl, mailpitUrl, std::format("kast-world-b-{}@example.com", stamp), password, ip(2))) {
        check(false, "live world: two verified accounts");
        return 1;
    }

    // The realm id, from the auth service's list.
    uint64_t realm = 0;
    const uint64_t realmsCall = a.auth.realms();
    a.waitFor([&] {
        for (const NetEvent& e : a.authEvents)
            if (e.name == "auth.realms" && e.payload.value("id", uint64_t{0}) == realmsCall)
                for (const Json& r : e.payload["realms"])
                    if (r.value("name", "") == realmName)
                        realm = r.value("id", uint64_t{0});
        return realm != 0;
    });
    if (!realm) {
        kast::log::warn(std::format("SKIPPED: no realm named '{}' (the ctest setup step adds it)", realmName));
        return kSkipped;
    }

    check(a.connect(realm), "live world: A connects (Hello, Auth with the session token -> world.connected)");
    a.world.listCharacters();
    const Json* none = a.next("world.charList");
    check(none && none->at("characters").empty(), "live world: a new account has no characters");
    const std::string heroA = uniqueName("Ka"), spare = uniqueName("Ks"), heroB = uniqueName("Kb");
    a.world.createCharacter(heroA);
    const Json* created = a.next("world.charCreated");
    check(created && created->at("character").value("name", "") == heroA, "live world: create a character");
    const uint64_t idA = created ? created->at("character").value("id", uint64_t{0}) : 0;
    a.world.createCharacter(heroA);
    const Json* taken = a.next("world.charError");
    check(taken && taken->value("code", "") == "name_taken" && taken->value("request", "") == "CharCreate",
          "live world: the same name again -> world.charError name_taken");
    a.world.createCharacter("x");
    const Json* invalid = a.next("world.charError");
    check(invalid && invalid->value("code", "") == "name_invalid", "live world: a 1-letter name -> name_invalid");
    a.world.createCharacter(spare);
    const Json* spareMade = a.next("world.charCreated");
    const uint64_t spareId = spareMade ? spareMade->at("character").value("id", uint64_t{0}) : 0;
    a.world.deleteCharacter(spareId);
    const Json* deleted = a.next("world.charDeleted");
    check(deleted && deleted->value("charId", uint64_t{0}) == spareId, "live world: delete a character");
    a.world.listCharacters();
    const Json* listed = a.next("world.charList");
    check(listed && listed->at("characters").size() == 1 && listed->at("characters")[0].value("id", uint64_t{0}) == idA,
          "live world: the list holds the live character only");

    // The server checks idleness on its heartbeat (every PING_INTERVAL_MS, 20 s), so a silent client is dropped within
    // IDLE_TIMEOUT_MS + 20 s = 80 s of its last message.
    kast::log::info("live world: idling in the lobby for 85 s (IDLE_TIMEOUT_MS 60 s + one 20 s heartbeat)");
    a.waitFor([&] { return a.count("world.disconnected") > 0; }, 85s);
    check(a.count("world.disconnected") == 0 && a.world.state() == kast::WorldState::Lobby,
          "live world: Ping answered, the idle connection survives past IDLE_TIMEOUT_MS");

    a.world.enterWorld(idA);
    const Json* enteredA = a.next("world.entered");
    check(enteredA && a.world.state() == kast::WorldState::InWorld, "live world: A enters the world");

    check(b.connect(realm), "live world: B connects");
    b.world.createCharacter(heroB);
    const Json* createdB = b.next("world.charCreated");
    b.world.enterWorld(createdB ? createdB->at("character").value("id", uint64_t{0}) : 0);
    const Json* enteredB = b.next("world.entered");
    check(enteredB != nullptr, "live world: B enters the world at the spawn point, next to A");
    const uint32_t entityB = enteredB ? enteredB->value("entityId", 0u) : 0;
    const Json* spawn = a.next("world.spawn");
    check(spawn && spawn->value("name", "") == heroB && spawn->value("entityId", 0u) == entityB,
          "live world: B's arrival is world.spawn {entityId, name} on A");
    const std::string text = "hello from b " + stamp;
    b.world.say(text);
    const Json* chat = a.next("world.chat");
    check(chat && chat->value("from", "") == heroB && chat->value("text", "") == text,
          "live world: B's say reaches A as world.chat {from, text}");
    b.world.disconnect();
    const Json* gone = a.next("world.despawn");
    check(gone && gone->value("entityId", 0u) == entityB, "live world: B's disconnect is world.despawn on A");
    a.world.disconnect();
    for (WorldRun* r : {&a, &b})
        for (const NetEvent& e : r->events)
            if (e.name == "world.disconnected" && e.payload.value("code", "") != "client_disconnect")
                kast::log::error(std::format("live world: {}", e.payload.dump()));
    check(std::ranges::none_of(kast::log::lines(),
                               [&](const std::string& line) {
                                   return contains(line, password) || contains(line, text) ||
                                          contains(line, a.auth.accessToken().value_or("no token")) ||
                                          contains(line, b.auth.accessToken().value_or("no token"));
                               }),
          "live world: no password, token or chat text in the log");
    if (g_failures == 0)
        kast::log::info("live world check passed");
    return g_failures == 0 ? 0 : 1;
}

int netSelfCheck()
{
    checkNetHelpers();
    checkNetTcpFraming();
    checkNetEncodings();
    checkNetTls();
    checkNetWebSocket();
    checkNetHttp();
    checkNetErrors();
    checkNetStop();
    checkNetLuaApi();
    checkNetSceneSwitch();
    checkAuthClient();
    checkAuthRefresh();
    checkAuthLuaApi();
    checkWorldClient();
    checkWorldLuaApi();
    checkAuthLogs();
    std::error_code ec;
    std::filesystem::remove_all(netRoot(), ec);
    return g_failures;
}
