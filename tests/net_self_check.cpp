// kast::Network, kast.net, kast::AuthClient and kast.auth checks for kast_self_check, against loopback servers (tests/net_test_util.h) on ephemeral
// ports with certificates made at run time. Kept out of self_check.cpp so only this file compiles Boost.Asio / Beast.
#include "net_test_util.h"

#include "auth_client.h"
#include "build.h"
#include "document.h"
#include "external_editor.h"
#include "online.h"
#include "log.h"
#include "math3d.h"
#include "lua_systems.h"
#include "network.h"
#include "play.h"
#include "project.h"
#include "scene_io.h"
#include "world_client.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <process.h>
#include <fstream>
#include <iterator>
#include <ranges>
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
    check(throwsLogic([&] { w.move(1, 2, 3, 0); }, "move: needs state 'inWorld'"), "world: move outside the world throws");

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
                  std::get_if<proto::Hello>(&seen[0])->protocolVersion == proto::kProtocolVersion &&
                  std::get_if<proto::Auth>(&seen[1]) && std::get_if<proto::Auth>(&seen[1])->accessToken == fake.accessToken(),
              "world: the handshake sends Hello{kProtocolVersion} then Auth{the session's access token}");
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
    check(hero && hero->at("character") ==
                      Json{{"id", 100}, {"name", "Hero"}, {"position", pos(100, 0, 100)}, {"options", Json::object()}},
          "world.charCreated {character = {id, name, position, options = {}}}");
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
    w.createCharacter("Mage", {{"class", "mage"}, {"hair", "red"}});
    const Json* mage = r.next("world.charCreated");
    check(mage && mage->at("character").at("options") == Json{{"class", "mage"}, {"hair", "red"}},
          "world: createCharacter options travel in CharCreate and come back in world.charCreated");
    w.listCharacters();
    const Json* two = r.next("world.charList");
    check(two && two->at("characters").size() == 2 && two->at("characters")[1].at("options") == Json{{"class", "mage"}, {"hair", "red"}},
          "world.charList entries carry options");

    w.enterWorld(100);
    const Json* entered = r.next("world.entered");
    check(entered && entered->value("entityId", 0) == 1 && entered->value("map", 0) == 1 &&
              entered->at("position") == pos(100, 0, 100) && entered->value("heading", 0.0) == 0.5 &&
              w.state() == kast::WorldState::InWorld,
          "world.entered {entityId, map, position, heading}; state inWorld");
    const Json* spawn = r.next("world.spawn");
    check(spawn && *spawn == Json{{"entityId", 2}, {"name", "Other"}, {"position", pos(101, 0, 100)}, {"heading", 0.0},
                                  {"kind", "player"}, {"template", ""}},
          "world.spawn {entityId, name, position, heading, kind player, template empty}");
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
    w.say("components");
    const Json* component = r.next("world.component");
    check(component && *component == Json{{"entityId", 2}, {"name", "Health"}, {"data", {{"current", 42.5}, {"max", 100}}}},
          "world.component {entityId, name, data}");
    const Json* removed = r.next("world.componentRemoved");
    check(removed && *removed == Json{{"entityId", 2}, {"name", "Health"}} && r.count("world.component") == 1,
          "world.componentRemoved {entityId, name}; a ComponentUpdate with invalid JSON is dropped");
    w.say("npc");
    const Json* npc = r.next("world.spawn");
    check(npc && *npc == Json{{"entityId", 3}, {"name", "Guard"}, {"position", pos(103, 0, 100)}, {"heading", 0.25},
                              {"kind", "npc"}, {"template", "Guard"}},
          "world.spawn of a v2 NPC Spawn: kind npc, its template");
    w.say("mapchange");
    const Json* mapChanged = r.next("world.mapChanged");
    check(mapChanged && *mapChanged == Json{{"map", 2}, {"position", pos(300, 0, 310)}, {"heading", 1.5}},
          "MapChange -> world.mapChanged {map, position, heading}");
    const Json* gone = r.next("world.despawn");
    check(gone && gone->value("entityId", 0) == 3 && w.state() == kast::WorldState::InWorld,
          "after world.mapChanged: the old neighbour's world.despawn; still inWorld");
    w.say("events");
    const Json* quest = r.next("server.quest.update");
    check(quest && *quest == Json::parse(R"({"quest": 7, "steps": [1, 2], "done": false})"),
          "ServerEvent -> server.<name> with the parsed payload");
    const Json* serverPing = r.next("server.ping");
    check(serverPing && serverPing->is_null() && r.count("server.broken") == 0 &&
              logged("World: ignored ServerEvent \"broken\" with invalid JSON"),
          "a ServerEvent with invalid JSON is warned and dropped; the next one still arrives");
    check(w.latest("world.mapChanged") && *w.latest("world.mapChanged") == *mapChanged &&
              w.latest("server.quest.update") && *w.latest("server.quest.update") == *quest && !w.latest("server.broken") &&
              !w.latest("server.never"),
          "world: latest(name) is the last payload of that event, null if none came");
    w.say("bye");
    const Json* despawn = r.next("world.despawn");
    check(despawn && *despawn == Json{{"entityId", 2}}, "world.despawn {entityId}");

    w.disconnect();
    const Json* bye = r.next("world.disconnected");
    check(bye && bye->value("code", "") == "client_disconnect" && w.state() == kast::WorldState::Disconnected,
          "world: disconnect -> world.disconnected client_disconnect");
    check(!w.latest("world.entered") && !w.latest("server.quest.update") && !w.latest("world.disconnected"),
          "world: world.disconnected forgets the connection's latest events");
    w.disconnect();
    r.pump();
    check(r.count("world.disconnected") == 3, "world: disconnect while disconnected publishes nothing");

    // Every code the server can send maps to its name in docs/services/world/protocol.html; an unknown value to "unknown".
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
                               "enterWorld", "say", "send", "state"}}) do
            assert(type(kast.world[name]) == "function", name .. " missing")
        end
        assert(kast.world.state() == "disconnected")
        fails("kast.world.connect: realmId must be a whole number >= 0, got x", kast.world.connect, "x")
        fails("kast.world.connect: realmId must be a whole number >= 0, got 1.5", kast.world.connect, 1.5)
        fails("kast.world.createCharacter: name must be a string, got nil", kast.world.createCharacter)
        fails("kast.world.createCharacter: options must be a table, got 5", kast.world.createCharacter, "Hero", 5)
        fails("kast.world.createCharacter: options must map strings to strings, got class = 1",
              kast.world.createCharacter, "Hero", {{class = 1}})
        fails("kast.world.say: needs state 'inWorld', the connection is 'disconnected'", kast.world.say, "hi")
        fails("kast.world.listCharacters: needs state 'lobby'", kast.world.listCharacters)
        kast.subscribe("world.disconnected", function(e) kast.log("lua world disconnected " .. e.code) end)
        kast.subscribe("world.connected", function(e)
            kast.log(string.format("lua world connected %d %s %s", e.accountId, e.role, kast.world.state()))
            kast.world.createCharacter("Hero", {{class = "mage"}})
        end)
        kast.subscribe("world.charCreated", function(e)
            kast.log("lua world created " .. e.character.name .. " " .. e.character.id .. " " .. e.character.options.class)
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
        local ok, err = pcall(kast.subscribe, "world.entered", function() end, 5)
        assert(not ok and string.find(err, "kast.subscribe: options must be a table, got 5", 1, true), tostring(err))
        local initializing = true
        kast.subscribe("world.connected", function(e)
            kast.log(string.format("lua world replayed connected %d in init %s", e.accountId, tostring(initializing)))
        end, {replay = true})
        kast.subscribe("world.charError", function() kast.log("lua world replayed nothing") end, {replay = true})
        initializing = false
        kast.subscribe("world.spawn", function(e) kast.log("lua world spawn " .. e.entityId .. " " .. e.name) end)
        kast.subscribe("world.chat", function(e) kast.log("lua world chat from " .. e.from) end)
        kast.subscribe("server.quest.update", function(e)
            kast.log(string.format("lua server event quest %d steps %d", e.quest, #e.steps))
        end)
        kast.subscribe("server.echo.quest.accept", function(e)
            kast.log(string.format("lua client event echoed quest %d", e.quest))
        end)
        kast.subscribe("world.despawn", function(e)
            kast.log("lua world despawn " .. e.entityId)
            kast.world.disconnect()
        end)
        kast.subscribe("world.disconnected", function(e)
            kast.log("lua world b disconnected " .. e.code .. " " .. kast.world.state())
        end)
        kast.world.say("events")
        kast.world.send("quest.accept", {quest = 7})
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
    check(playUntil(doc, "lua world created Hero 100 mage"), "kast.world.createCharacter(name, options) -> world.charCreated with options");
    check(playUntil(doc, "lua world entered 1 at 100,0,100"), "kast.world.enterWorld -> world.entered");
    check(playUntil(doc, "lua world replayed connected 42 in init true") && !logged("lua world replayed nothing"),
          "kast.subscribe(name, fn, {replay = true}): a later scene gets the connection's last event of that name at once");
    check(playUntil(doc, "lua world chat from Hero"), "kast.world.say -> world.chat (after a scene switch)");
    check(playUntil(doc, "lua server event quest 7 steps 2"), "kast.subscribe(\"server.<name>\") gets the ServerEvent payload");
    check(playUntil(doc, "lua client event echoed quest 7"), "kast.world.send -> ClientEvent{name, JSON object}");
    check(playUntil(doc, "lua world despawn 2"), "world.despawn");
    check(playUntil(doc, "lua world b disconnected client_disconnect disconnected"),
          "kast.world.disconnect -> world.disconnected client_disconnect");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        if (!s.ok)
            kast::log::error(std::format("  {}: {}", s.path, s.error));
    check(netMessages == 0, "kast.world's frames are not published as net.message");
    bool fromWorld = false, serverFromWorld = false;
    std::string history;
    for (const kast::EventBus::Delivered& d : doc.lua.bus().history()) {
        fromWorld |= d.name == "world.connected" && d.publisher == "kast.world";
        serverFromWorld |= d.name == "server.quest.update" && d.publisher == "kast.world";
        history += d.payload;
    }
    check(fromWorld, "world.* events are published by \"kast.world\"");
    check(serverFromWorld, "server.* events are published by \"kast.world\"");
    check(!contains(history, fake.accessToken()), "no event payload holds the access token");
    stopPlay(doc);
    check(!doc.lua.world().enabled() && doc.lua.world().state() == kast::WorldState::Disconnected,
          "Stop drops the world connection");
}

// kast.world.send's checks, all before any connection (no network): name and size are checked before the state.
void checkWorldSendOffline()
{
    const std::filesystem::path dir = netProject("kast_world_send_check");
    writeFile(dir / kast::kProjectFileName, R"({"version": 1, "name": "Send", "startScene": null,
"online": {"enabled": true, "authUrl": "http://127.0.0.1:9"}})");
    saveScene(dir / "a.scene", {"scripts/a.lua"});
    writeFile(dir / "scripts/a.lua", R"(local function fails(part, fn, ...)
    local ok, err = pcall(fn, ...)
    assert(not ok and string.find(err, part, 1, true), "expected '" .. part .. "', got " .. tostring(err))
end
return {
    init = function()
        local badName = "kast.world.send: name must be 1-32 characters of a-z, 0-9, '.' and '_', got '"
        fails("kast.world.send: name must be a string, got nil", kast.world.send)
        fails(badName .. "'", kast.world.send, "")
        fails(badName .. "Combat.attack'", kast.world.send, "Combat.attack")
        fails(badName .. "a b'", kast.world.send, "a b")
        fails(badName .. string.rep("a", 33) .. "'", kast.world.send, string.rep("a", 33))
        fails("kast.world.send: data must be a table, got 5", kast.world.send, "x", 5)
        fails("kast.world.send: data must be a table with string keys, not a list", kast.world.send, "x", {1, 2})
        fails("kast.world.send: data is 1032 bytes encoded, the limit is 1024", kast.world.send, "x",
              {s = string.rep("a", 1024)})
        fails("kast.world.send: needs state 'inWorld', the connection is 'disconnected'", kast.world.send,
              string.rep("a", 32), {s = string.rep("a", 1016)}) -- 32 chars and exactly 1024 bytes pass the checks
        fails("kast.world.send: needs state 'inWorld'", kast.world.send, "combat.attack_2", {})
        fails("kast.world.send: needs state 'inWorld'", kast.world.send, "combat.attack")
        kast.log("lua world send checks done")
    end,
    update = function(dt) end,
}
)");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "open the kast.world.send scene");
    startPlay(doc);
    check(playUntil(doc, "lua world send checks done"),
          "kast.world.send raises for a bad name, non-table data, a list, > 1 KB encoded, and outside inWorld");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        check(s.ok, "kast.world.send checks: " + s.path + " " + s.error);
    stopPlay(doc);
}

// --- Replication (engine/replication.h) ---
namespace {

bool approx(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }
bool approx(const bx::Vec3& a, const bx::Vec3& b, float tolerance = 1e-4f)
{
    return approx(a.x, b.x, tolerance) && approx(a.y, b.y, tolerance) && approx(a.z, b.z, tolerance);
}

entt::entity named(const kast::Scene& scene, const std::string& name)
{
    entt::entity found = entt::null;
    for (auto [e, n] : scene.registry().view<kast::Name>().each())
        if (n.value == name)
            found = e;
    return found;
}

int countNamed(const kast::Scene& scene, const std::string& name)
{
    int n = 0;
    for (auto [e, value] : scene.registry().view<kast::Name>().each())
        n += value.value == name;
    return n;
}

std::vector<proto::Move> moves(const FakeWorld& fake)
{
    std::vector<proto::Move> out;
    for (const proto::Message& m : fake.seen())
        if (const auto* move = std::get_if<proto::Move>(&m))
            out.push_back(*move);
    return out;
}

Json at(float x, float y, float z) { return {{"x", x}, {"y", y}, {"z", z}}; }

} // namespace

// The pure helpers and the ECS plumbing driven by hand-published world.* events (no network).
void checkReplicationOffline()
{
    using kast::interpolate;
    const bx::Vec3 a{0, 0, 0}, b{10, 2, -4};
    check(approx(interpolate(a, b, 0.0f, 0.1f), a) && approx(interpolate(a, b, 0.05f, 0.1f), {5, 1, -2}) &&
              approx(interpolate(a, b, 0.1f, 0.1f), b) && approx(interpolate(a, b, 0.5f, 0.1f), b) &&
              approx(interpolate(a, b, -1.0f, 0.1f), a) && approx(interpolate(a, b, 0.0f, 0.0f), b),
          "replication: interpolate is a straight line over the duration, held at the target, 0 snaps");
    const float pi = 3.14159265f;
    bool turns = true;
    for (float h : {0.0f, 0.5f, -1.0f, pi / 2, 3.0f})
        turns &= approx(kast::headingOf(kast::yawRotation(h)), h);
    float m[16];
    kast::mtxFromQuat(m, kast::yawRotation(pi / 2));
    check(turns && approx(kast::transformDir({0, 0, 1}, m), {1, 0, 0}) &&
              approx(kast::headingOf(bx::Quaternion{bx::InitIdentity}), 0.0f),
          "replication: heading <-> yaw round trip, heading pi/2 faces +X (atan2(dx, dz))");

    kast::MoveThrottle throttle;
    check(throttle.shouldSend({1, 0, 1}, 0, 0.0, 10) && !throttle.shouldSend({1, 0, 1}, 0, 1.0, 10),
          "replication: the first position is sent, an unchanged one never");
    check(throttle.shouldSend({2, 0, 1}, 0, 1.05, 10), "replication: a change after the interval is sent");
    check(!throttle.shouldSend({3, 0, 1}, 0, 1.1, 10) && throttle.shouldSend({3, 0, 1}, 0, 1.16, 10),
          "replication: a change within 1/rate waits for the interval");
    check(!throttle.shouldSend({3.00001f, 0, 1}, 0, 5.0, 10) && throttle.shouldSend({3, 0, 1}, 0.2f, 5.0, 10),
          "replication: sub-0.1 mm jitter is not a move; a heading change is");
    throttle.known({7, 0, 7}, 0.2f);
    check(!throttle.shouldSend({7, 0, 7}, 0.2f, 9.0, 10), "replication: a server-known position is not sent back");
    for (float rate : {10.0f, 30.0f}) {
        kast::MoveThrottle t;
        int sent = 0;
        for (int frame = 0; frame < 600; ++frame) // 10 s at 60 fps, moving every frame
            sent += t.shouldSend({frame * 0.05f, 0, 0}, 0, frame / 60.0, rate);
        int idle = 0;
        for (int frame = 600; frame < 1200; ++frame) // then 10 s standing still
            idle += t.shouldSend({599 * 0.05f, 0, 0}, 0, frame / 60.0, rate);
        check(sent == static_cast<int>(rate) * 10 && idle == 1,
              std::format("replication: {} Hz while moving: {} sends in 10 s (want {}), {} while idle (want 1: the "
                          "final position)", rate, sent, static_cast<int>(rate) * 10, idle));
    }

    // Components: registered, round trip, validation.
    kast::Scene scene;
    std::string error;
    for (const char* name : {"NetworkLocalPlayer", "NetworkReplication", "NetworkEntity"})
        check(static_cast<bool>(kast::findAnyComponentType(name)), std::string("replication: ") + name + " is registered");
    const entt::entity probe = scene.create("probe");
    const kast::ComponentType local = kast::findAnyComponentType("NetworkLocalPlayer");
    const kast::ComponentType manager = kast::findAnyComponentType("NetworkReplication");
    check(kast::setComponentJson(scene.registry(), probe, local, {{"sendRateHz", 20}}, error) &&
              !kast::setComponentJson(scene.registry(), probe, local, {{"sendRateHz", 31}}, error) &&
              !kast::setComponentJson(scene.registry(), probe, local, {{"sendRateHz", 0.5}}, error) &&
              kast::componentToJson(scene.registry(), probe, local) == nlohmann::ordered_json{{"sendRateHz", 20}},
          "replication: sendRateHz 1..30, out of range rejected and the value kept");
    check(kast::setComponentJson(scene.registry(), probe, manager, {{"templateEntity", "T"}, {"interpolationMs", 50}}, error) &&
              !kast::setComponentJson(scene.registry(), probe, manager, {{"templateEntity", "T"}, {"interpolationMs", -1}}, error) &&
              kast::componentToJson(scene.registry(), probe, manager) ==
                  nlohmann::ordered_json{{"templateEntity", "T"}, {"interpolationMs", 50}, {"npcTemplates", ""}},
          "replication: NetworkReplication round trip (npcTemplates optional), negative interpolation rejected");
    check(kast::setComponentJson(scene.registry(), probe, manager,
                                 {{"templateEntity", "T"}, {"interpolationMs", 50}, {"npcTemplates", "NPCs"}}, error) &&
              kast::componentToJson(scene.registry(), probe, manager).value("npcTemplates", "") == "NPCs",
          "replication: NetworkReplication npcTemplates round trip");

    // The ECS plumbing on hand-published events.
    const auto build = [](kast::Scene& s) {
        const entt::entity player = s.create("Player");
        s.registry().emplace<kast::NetworkLocalPlayer>(player);
        const entt::entity templ = s.create("Template");
        s.registry().get<kast::Transform>(templ).scale = {2, 2, 2};
        const entt::entity body = s.create("Body", templ);
        s.registry().emplace<kast::MeshRenderer>(body, kast::MeshRenderer{.primitive = kast::Primitive::Sphere});
        const entt::entity m = s.create("Manager");
        s.registry().emplace<kast::NetworkReplication>(m, kast::NetworkReplication{"Template", 100.0f});
        return player;
    };
    {
        Network net;
        kast::AuthClient auth{net};
        kast::WorldClient world{net, auth}; // disabled
        kast::EventBus bus;
        kast::Scene s;
        build(s);
        kast::Replication r;
        r.start(s, bus, world);
        bus.publish("world.spawn", {{"entityId", 2}, {"name", "Other"}, {"position", at(1, 0, 1)}, {"heading", 0}});
        bus.drain();
        r.update(0.1f);
        check(named(s, "Template") != entt::null && named(s, "Other") == entt::null && r.clone(2) == entt::null,
              "replication: Online Services disabled -> the template stays, world events do nothing");
    }
    Network net;
    kast::AuthClient auth{net};
    kast::WorldClient world{net, auth};
    world.reset(kast::AuthConfig{"http://127.0.0.1:1", ""});
    kast::EventBus bus;
    kast::Scene s;
    const entt::entity player = build(s);
    kast::Replication r;
    r.start(s, bus, world);
    check(named(s, "Template") == entt::null && named(s, "Body") == entt::null,
          "replication: the template subtree leaves the scene at start");
    std::vector<Json> spawned;
    kast::EventBus::Subscription sub = bus.subscribe("world.entitySpawned", [&](const Json& e) { spawned.push_back(e); });
    bus.publish("world.entered", {{"entityId", 1}, {"position", at(50, 1, 60)}, {"heading", 0.5}});
    bus.publish("world.spawn", {{"entityId", 2}, {"name", "Other"}, {"position", at(10, 0, 10)}, {"heading", 1.0}});
    bus.publish("world.spawn", {{"entityId", 3}, {"name", "Third"}, {"position", at(20, 0, 20)}, {"heading", 0}});
    bus.drain();
    const kast::Transform& pt = s.registry().get<kast::Transform>(player);
    check(approx(pt.position, {50, 1, 60}) && approx(kast::headingOf(pt.rotation), 0.5f),
          "replication: world.entered places the local player (position and heading)");
    {
        // world.mapChanged: remote clones gone, the local player placed; spawns after it clone again
        kast::EventBus mbus; // its own bus: the outer Replication must not see these
        kast::Scene ms;
        const entt::entity localPlayer = build(ms);
        kast::Replication mr;
        mr.start(ms, mbus, world);
        mbus.publish("world.entered", {{"entityId", 1}, {"map", 1}, {"position", at(50, 1, 60)}, {"heading", 0.5}});
        mbus.publish("world.spawn", {{"entityId", 7}, {"name", "Old"}, {"position", at(10, 0, 10)}, {"heading", 0}});
        mbus.drain();
        const entt::entity old = mr.clone(7);
        mbus.publish("world.mapChanged", {{"map", 2}, {"position", at(300, 0, 310)}, {"heading", 1.5}});
        mbus.drain();
        const kast::Transform& lt = ms.registry().get<kast::Transform>(localPlayer);
        check(ms.valid(old) == false && mr.clone(7) == entt::null && approx(lt.position, {300, 0, 310}) &&
                  approx(kast::headingOf(lt.rotation), 1.5f),
              "replication: world.mapChanged clears remote clones and places the local player");
        mbus.publish("world.spawn", {{"entityId", 8}, {"name", "New"}, {"position", at(305, 0, 310)}, {"heading", 0}});
        mbus.drain();
        check(ms.valid(mr.clone(8)), "replication: a spawn after world.mapChanged clones again");
        mr.stop();
    }
    const entt::entity other = r.clone(2);
    const auto& reg = s.registry();
    check(s.valid(other) && reg.get<kast::Name>(other).value == "Other" && reg.get<kast::NetworkEntity>(other).entityId == 2 &&
              approx(reg.get<kast::Transform>(other).position, {10, 0, 10}) &&
              approx(reg.get<kast::Transform>(other).scale, {2, 2, 2}) &&
              approx(kast::headingOf(reg.get<kast::Transform>(other).rotation), 1.0f) &&
              reg.get<kast::Hierarchy>(other).children.size() == 1 &&
              reg.get<kast::MeshRenderer>(reg.get<kast::Hierarchy>(other).children[0]).primitive == kast::Primitive::Sphere,
          "replication: world.spawn clones the template subtree at the spawn position (snapped), with NetworkEntity");
    bus.drain();
    check(spawned.size() == 2 && spawned[0].value("entityId", 0) == 2 && spawned[0].value("name", "") == "Other" &&
              spawned[0].value("entity", uint64_t{0}) == reg.get<kast::PersistentId>(other).value &&
              spawned[0].value("kind", "") == "player" && spawned[0].value("template", "x") == "",
          "replication: world.entitySpawned {entityId, entity, name, kind, template} follows each clone");
    bus.publish("world.entityMove", {{"entityId", 2}, {"position", at(20, 0, 10)}, {"heading", 1.0}});
    bus.drain();
    r.update(0.025f);
    const float quarter = reg.get<kast::Transform>(other).position.x;
    r.update(0.025f);
    const float half = reg.get<kast::Transform>(other).position.x;
    r.update(0.1f);
    const float done = reg.get<kast::Transform>(other).position.x;
    check(approx(quarter, 12.5f) && approx(half, 15.0f) && approx(done, 20.0f),
          std::format("replication: world.entityMove interpolates over 100 ms (want 12.5, 15, 20; got {}, {}, {})",
                      quarter, half, done));
    bus.publish("world.correction", {{"position", at(49, 1, 60)}});
    bus.drain();
    check(approx(pt.position, {49, 1, 60}) && approx(kast::headingOf(pt.rotation), 0.5f),
          "replication: world.correction snaps the local player's position, heading kept");
    bus.publish("world.despawn", {{"entityId", 2}});
    bus.drain();
    check(r.clone(2) == entt::null && !s.valid(other) && named(s, "Other") == entt::null && countNamed(s, "Body") == 1,
          "replication: world.despawn destroys that clone and its children");
    bus.publish("world.disconnected", {{"code", "connection_closed"}, {"reason", "x"}});
    bus.drain();
    check(r.clone(3) == entt::null && countNamed(s, "Body") == 0 && s.valid(player),
          "replication: world.disconnected destroys every clone; the local player stays");

    // A scene switch keeps the remote players in view: the new scene gets their clones.
    bus.publish("world.spawn", {{"entityId", 4}, {"name", "Fourth"}, {"position", at(5, 0, 5)}, {"heading", 0}});
    bus.drain();
    r.stop(true);
    kast::Scene next;
    build(next);
    r.start(next, bus, world);
    check(next.valid(r.clone(4)) && next.registry().get<kast::Name>(r.clone(4)).value == "Fourth" &&
              approx(next.registry().get<kast::Transform>(r.clone(4)).position, {5, 0, 5}),
          "replication: after a scene switch the remote players in view are cloned into the new scene");
    r.stop();
    check(r.clone(4) == entt::null, "replication: stop() forgets them");
}

// NPC spawns (world.spawn kind "npc"): clones of their own template, a child of NetworkReplication.npcTemplates, which
// leaves the scene at start with every template under it; an empty or missing template falls back to the template
// entity with one warning per name. Players are unchanged.

// A template entity that is a prefab instance clones like any other: its components and subtree, with no prefab link.
void checkReplicationPrefabTemplate()
{
    const std::filesystem::path dir = netProject("replication_prefab");
    kast::Scene prefab;
    const entt::entity root = prefab.create("Template");
    prefab.registry().get<kast::Transform>(root).scale = {2, 2, 2};
    const entt::entity body = prefab.create("Body", root);
    prefab.registry().emplace<kast::MeshRenderer>(body, kast::MeshRenderer{.primitive = kast::Primitive::Sphere});
    std::string error;
    check(kast::savePrefab(prefab, dir, "prefabs/template.prefab", error), "replication prefab: write the template prefab");
    std::shared_ptr<const kast::PrefabAsset> asset;
    kast::Scene scene;
    check(kast::loadPrefab(dir, "prefabs/template.prefab", asset, error), "replication prefab: load it");
    const entt::entity instance = kast::instantiatePrefab(scene, asset, entt::null, error);
    scene.registry().get<kast::Transform>(instance).position = {5, 0, 5}; // an instance override
    const entt::entity m = scene.create("Manager");
    scene.registry().emplace<kast::NetworkReplication>(m, kast::NetworkReplication{"Template", 100.0f});
    scene.registry().emplace<kast::NetworkLocalPlayer>(scene.create("Player"));
    check(kast::saveScene(scene, (dir / "r.scene").string(), error), "replication prefab: save the scene");
    kast::Scene s;
    check(kast::loadScene((dir / "r.scene").string(), s, error, nullptr, dir) && named(s, "Template") != entt::null &&
              kast::isPrefabRoot(s, named(s, "Template")),
          "replication prefab: the template loads as an instance");

    Network net;
    kast::AuthClient auth{net};
    kast::WorldClient world{net, auth};
    world.reset(kast::AuthConfig{"http://127.0.0.1:1", ""});
    kast::EventBus bus;
    kast::Replication r;
    r.start(s, bus, world);
    bus.publish("world.spawn", {{"entityId", 2}, {"name", "Other"}, {"position", at(10, 0, 10)}, {"heading", 0}});
    bus.drain();
    const entt::entity clone = r.clone(2);
    const auto& kids = s.valid(clone) ? s.children(clone) : std::vector<entt::entity>{};
    check(named(s, "Template") == entt::null && s.valid(clone) && kids.size() == 1 &&
              s.registry().all_of<kast::MeshRenderer>(kids[0]) &&
              approx(s.registry().get<kast::Transform>(clone).scale, {2, 2, 2}) &&
              !s.registry().any_of<kast::PrefabLink, kast::PrefabInstance>(clone),
          "replication prefab: the clone copies the instance's subtree and components, unlinked");
    r.stop();
}
void checkReplicationNpcs()
{
    const auto build = [](kast::Scene& s) {
        s.registry().emplace<kast::NetworkLocalPlayer>(s.create("Player"));
        const entt::entity templ = s.create("Template");
        s.registry().emplace<kast::MeshRenderer>(s.create("Body", templ),
                                                 kast::MeshRenderer{.primitive = kast::Primitive::Sphere});
        const entt::entity folder = s.create("NPC Templates");
        const entt::entity guard = s.create("Guard", folder);
        s.registry().get<kast::Transform>(guard).scale = {3, 3, 3};
        s.registry().emplace<kast::MeshRenderer>(s.create("Guard Body", guard),
                                                 kast::MeshRenderer{.primitive = kast::Primitive::Cube});
        s.create("Merchant", folder);
        s.registry().emplace<kast::NetworkReplication>(s.create("Manager"),
                                                       kast::NetworkReplication{"Template", 100.0f, "NPC Templates"});
    };
    const auto childPrimitive = [](const kast::Scene& s, entt::entity e) {
        const auto& children = s.registry().get<kast::Hierarchy>(e).children;
        return children.size() == 1 && s.registry().all_of<kast::MeshRenderer>(children[0])
                   ? s.registry().get<kast::MeshRenderer>(children[0]).primitive
                   : kast::Primitive::Plane; // no single rendered child
    };
    const auto warnings = [](const std::string& part) {
        return std::ranges::count_if(kast::log::lines(), [&](const std::string& line) { return contains(line, part); });
    };
    Network net;
    kast::AuthClient auth{net};
    kast::WorldClient world{net, auth};
    world.reset(kast::AuthConfig{"http://127.0.0.1:1", ""});
    kast::EventBus bus;
    kast::Scene s;
    build(s);
    kast::Replication r;
    r.start(s, bus, world);
    check(named(s, "NPC Templates") == entt::null && named(s, "Guard") == entt::null &&
              named(s, "Guard Body") == entt::null && named(s, "Merchant") == entt::null &&
              named(s, "Template") == entt::null && s.valid(named(s, "Player")),
          "replication npcs: the npcTemplates entity and every template under it leave the scene at start");

    const auto spawnNpc = [&](uint32_t id, const std::string& name, const std::string& templ) {
        bus.publish("world.spawn", {{"entityId", id}, {"name", name}, {"position", at(30, 0, 40)}, {"heading", 0.5},
                                    {"kind", "npc"}, {"template", templ}});
        bus.drain();
        return r.clone(id);
    };
    const entt::entity guard = spawnNpc(10, "Guard", "Guard");
    const auto& reg = s.registry();
    check(s.valid(guard) && reg.get<kast::Name>(guard).value == "Guard" &&
              reg.get<kast::NetworkEntity>(guard).entityId == 10 && approx(reg.get<kast::Transform>(guard).scale, {3, 3, 3}) &&
              approx(reg.get<kast::Transform>(guard).position, {30, 0, 40}) &&
              approx(kast::headingOf(reg.get<kast::Transform>(guard).rotation), 0.5f) &&
              childPrimitive(s, guard) == kast::Primitive::Cube,
          "replication npcs: an NPC spawn clones its own template's subtree at the spawn position, with NetworkEntity");
    const entt::entity merchant = spawnNpc(11, "Bob the Merchant", "Merchant");
    check(s.valid(merchant) && reg.get<kast::Name>(merchant).value == "Bob the Merchant" &&
              reg.get<kast::Hierarchy>(merchant).children.empty(),
          "replication npcs: each template by its name; the clone is named after the NPC");

    const auto before = warnings("no NPC template 'Ghost'");
    const entt::entity ghost1 = spawnNpc(12, "Ghost", "Ghost");
    const entt::entity ghost2 = spawnNpc(13, "Ghost", "Ghost");
    const entt::entity unnamed = spawnNpc(14, "Nameless", "");
    check(s.valid(ghost1) && s.valid(ghost2) && childPrimitive(s, ghost1) == kast::Primitive::Sphere &&
              childPrimitive(s, ghost2) == kast::Primitive::Sphere && childPrimitive(s, unnamed) == kast::Primitive::Sphere,
          "replication npcs: a missing or empty template falls back to the template entity");
    check(warnings("no NPC template 'Ghost'") - before == 1 && warnings("no NPC template ''") == 1,
          "replication npcs: one warning per missing template name");

    bus.publish("world.spawn", {{"entityId", 20}, {"name", "Guard"}, {"position", at(1, 0, 1)}, {"heading", 0},
                                {"kind", "player"}, {"template", ""}});
    bus.publish("world.spawn", {{"entityId", 21}, {"name", "Other"}, {"position", at(2, 0, 2)}, {"heading", 0}});
    bus.drain();
    check(childPrimitive(s, r.clone(20)) == kast::Primitive::Sphere && childPrimitive(s, r.clone(21)) == kast::Primitive::Sphere,
          "replication npcs: players (kind player, or no kind) still clone the template entity, even when named "
          "like an NPC template");

    bus.publish("world.entityMove", {{"entityId", 10}, {"position", at(34, 0, 40)}, {"heading", 1.0}});
    bus.drain();
    r.update(0.2f);
    check(approx(reg.get<kast::Transform>(guard).position, {34, 0, 40}), "replication npcs: world.entityMove moves an NPC clone");
    bus.publish("world.despawn", {{"entityId", 10}});
    bus.drain();
    check(r.clone(10) == entt::null && !s.valid(guard) && countNamed(s, "Guard Body") == 0,
          "replication npcs: world.despawn destroys the NPC clone");

    // A scene switch: the NPCs in view are cloned again from the new scene's templates.
    r.stop(true);
    kast::Scene next;
    build(next);
    r.start(next, bus, world);
    check(next.valid(r.clone(11)) && named(next, "Merchant") == entt::null &&
              next.registry().get<kast::Name>(r.clone(11)).value == "Bob the Merchant",
          "replication npcs: after a scene switch NPCs are cloned from the new scene's templates");
    r.stop();

    // Editor Play / Stop on tests/project's online_npcs.scene (its script left out: no network here).
    const std::filesystem::path dir = netProject("kast_replication_npcs_check");
    writeFile(dir / kast::kProjectFileName, R"({"version": 1, "name": "Npcs", "startScene": null,
"online": {"enabled": true, "authUrl": "http://127.0.0.1:1"}})");
    std::filesystem::copy_file(std::filesystem::path(KAST_TEST_PROJECT_DIR) / "online_npcs.scene", dir / "online_npcs.scene");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "online_npcs.scene").string(), error) && doc.project,
          "replication npcs: open online_npcs.scene in an online project " + error);
    doc.scene.systems.clear();
    const nlohmann::ordered_json saved = kast::sceneToJson(doc.scene);
    startPlay(doc);
    check(named(doc.scene, "NPC Templates") == entt::null && named(doc.scene, "Guard") == entt::null &&
              named(doc.scene, "Guard Body") == entt::null && named(doc.scene, "Remote Player") == entt::null,
          "replication npcs: the NPC templates are out of the Play scene");
    doc.lua.bus().publish("world.spawn", {{"entityId", 7}, {"name", "Guard"}, {"position", at(1030, 0, 1020)},
                                          {"heading", 0}, {"kind", "npc"}, {"template", "Guard"}});
    updatePlay(doc, 0.016f);
    const entt::entity clone = doc.lua.replication().clone(7);
    check(doc.scene.valid(clone) && doc.scene.registry().get<kast::Hierarchy>(clone).children.size() == 2 &&
              named(doc.scene, "Guard Helmet") != entt::null,
          "replication npcs: the Guard spawn is a clone of the scene's Guard template (body and helmet)");
    stopPlay(doc);
    check(kast::sceneToJson(doc.scene) == saved && named(doc.scene, "Guard") != entt::null &&
              named(doc.scene, "NPC Templates") != entt::null && doc.scene.valid(named(doc.scene, "Guard Helmet")),
          "replication npcs: Stop restores the NPC templates and the whole scene");
}

// Replicated Lua components (world.component / world.componentRemoved) applied by Replication to the local player and
// clones, with tests/project's Health definition (shared with services/world/lua).
void checkReplicationComponents()
{
    kast::activateComponentDefinitions(std::filesystem::path(KAST_TEST_PROJECT_DIR), nullptr);
    const kast::ComponentType health = kast::findAnyComponentType("Health");
    check(health.lua() != nullptr, "replication components: tests/project defines Health (persist/replicate accepted)");
    if (!health.lua())
        return;
    const auto build = [](kast::Scene& s) {
        const entt::entity player = s.create("Player");
        s.registry().emplace<kast::NetworkLocalPlayer>(player);
        s.create("Template");
        s.registry().emplace<kast::NetworkReplication>(s.create("Manager"), kast::NetworkReplication{"Template", 100.0f});
        return player;
    };
    Network net;
    kast::AuthClient auth{net};
    kast::WorldClient world{net, auth};
    world.reset(kast::AuthConfig{"http://127.0.0.1:1", ""});
    kast::EventBus bus;
    kast::Scene s;
    const entt::entity player = build(s);
    kast::Replication r;
    r.start(s, bus, world);
    bus.publish("world.entered", {{"entityId", 1}, {"position", at(0, 0, 0)}, {"heading", 0}});
    bus.publish("world.spawn", {{"entityId", 2}, {"name", "Other"}, {"position", at(1, 0, 1)}, {"heading", 0}});
    bus.drain();
    const auto& reg = s.registry();
    const auto value = [&](entt::entity e) {
        return kast::hasComponent(reg, e, health) ? kast::componentToJson(reg, e, health) : nlohmann::ordered_json();
    };
    const Json full = Json::parse(R"({"current":30,"max":100,"regen":true,"lives":3,"title":"Hero","tint":[1,0,0],
        "facing":[0,0,0,1],"faction":"ally","drops":["gold"]})");
    bus.publish("world.component", {{"entityId", 2}, {"name", "Health"}, {"data", full}});
    bus.drain();
    const entt::entity other = r.clone(2);
    check(value(other)["current"] == 30.0 && value(other)["faction"] == "ally" && value(other)["drops"].size() == 1,
          "replication: world.component adds the Lua component to the clone with the server's values");
    Json changed = full;
    changed["current"] = 12.5;
    bus.publish("world.component", {{"entityId", 2}, {"name", "Health"}, {"data", changed}});
    bus.publish("world.component", {{"entityId", 1}, {"name", "Health"}, {"data", {{"current", 40}, {"lives", 99}}}});
    bus.drain();
    check(value(other)["current"] == 12.5, "replication: a later world.component replaces the clone's value");
    check(value(player)["current"] == 40.0 && value(player)["lives"] == 3 && value(player)["max"] == 100.0,
          "replication: the local player gets its own components, with drift rules (missing -> default, 99 > max -> default)");
    check(logged("Replication: Entity 'Player'") && logged("\"lives\" 99 must be <= 9"), "replication: drift fixes are warned");
    for (int i = 0; i < 3; ++i)
        bus.publish("world.component", {{"entityId", 2}, {"name", "NoSuchComponent"}, {"data", Json::object()}});
    bus.drain();
    const auto unknownWarnings = std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
        return contains(line, "the server sent component \"NoSuchComponent\"");
    });
    check(unknownWarnings == 1 && s.valid(other), "replication: a name with no definition warns once and is ignored");

    // A scene switch: the new scene's player and clones get the last replicated values.
    r.stop(true);
    kast::Scene next;
    const entt::entity nextPlayer = build(next);
    r.start(next, bus, world);
    const auto& nreg = next.registry();
    check(kast::hasComponent(nreg, r.clone(2), health) && kast::componentToJson(nreg, r.clone(2), health)["current"] == 12.5 &&
              kast::hasComponent(nreg, nextPlayer, health) && kast::componentToJson(nreg, nextPlayer, health)["current"] == 40.0,
          "replication: replicated components survive a scene switch");
    bus.publish("world.componentRemoved", {{"entityId", 2}, {"name", "Health"}});
    bus.publish("world.componentRemoved", {{"entityId", 1}, {"name", "Health"}});
    bus.drain();
    check(!kast::hasComponent(nreg, r.clone(2), health) && !kast::hasComponent(nreg, nextPlayer, health),
          "replication: world.componentRemoved removes it from the clone and the local player");
    r.stop();

    // Entering from a scene with no local player and no template (character select), then switching to a zone scene:
    // the zone's player gets the server's position, heading and components, its clones the players already in view.
    kast::EventBus lobbyBus;
    kast::Scene lobby;
    lobby.create("Characters");
    kast::Replication lr;
    lr.start(lobby, lobbyBus, world);
    lobbyBus.publish("world.entered", {{"entityId", 1}, {"map", 10}, {"position", at(140, 5, 120)}, {"heading", 1.0}});
    lobbyBus.publish("world.spawn", {{"entityId", 2}, {"name", "Other"}, {"position", at(141, 5, 120)}, {"heading", 0}});
    lobbyBus.publish("world.component", {{"entityId", 1}, {"name", "Health"}, {"data", {{"current", 25}}}});
    lobbyBus.drain();
    lr.stop(true);
    kast::Scene zone;
    const entt::entity zonePlayer = build(zone);
    std::vector<std::string> told; // what the zone's scripts would hear
    kast::EventBus::Subscription tellEntered = lobbyBus.subscribe("world.entered", [&](const Json& e) {
        told.push_back(std::format("entered {} map {} at {},{},{}", e.value("entityId", 0), e.value("map", 0),
                                   e["position"].value("x", 0.0f), e["position"].value("y", 0.0f),
                                   e["position"].value("z", 0.0f)));
    });
    kast::EventBus::Subscription tellComponent = lobbyBus.subscribe("world.component", [&](const Json& e) {
        told.push_back(std::format("component {} {}", e.value("entityId", 0), e.value("name", "")));
    });
    lr.start(zone, lobbyBus, world);
    lobbyBus.drain();
    check((told == std::vector<std::string>{"entered 1 map 10 at 140,5,120", "component 1 Health"}),
          "replication: after a switch the new scene hears world.entered (map, pose now) and the replicated values again");
    const auto& zreg = zone.registry();
    const kast::Transform& zt = zreg.get<kast::Transform>(zonePlayer);
    check(approx(zt.position, {140, 5, 120}) && approx(kast::headingOf(zt.rotation), 1.0f),
          "replication: world.entered in a scene with no local player places the next scene's player");
    check(kast::hasComponent(zreg, zonePlayer, health) && kast::componentToJson(zreg, zonePlayer, health)["current"] == 25.0,
          "replication: the local player's components from a scene with no local player reach the next scene's");
    check(zone.valid(lr.clone(2)) && approx(zreg.get<kast::Transform>(lr.clone(2)).position, {141, 5, 120}),
          "replication: a remote player spawned in a scene with no template is cloned in the next scene");
    lobbyBus.publish("world.disconnected", {{"code", "connection_closed"}, {"reason", "x"}});
    lobbyBus.drain();
    lr.stop(true);
    kast::Scene again;
    const entt::entity againPlayer = build(again);
    lr.start(again, lobbyBus, world);
    check(approx(again.registry().get<kast::Transform>(againPlayer).position, {0, 0, 0}),
          "replication: after world.disconnected a new scene's player is not placed");
    lr.stop();
    kast::activateComponentDefinitions(std::nullopt, nullptr);
}

// Replication through LuaSystems against the fake world server: tests/project/online_replication.scene in an
// online-enabled copy, with its demo script logging in and entering the world.
void checkReplicationFakeServer()
{
    FakeAuth fake;
    Server authServer(serveFakeAuth(fake));
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    FakeWorld fakeWorld;
    Server worldServer(serveFakeWorld(fakeWorld), 0, &cert);
    fake.worldPort = worldServer.port();
    const std::filesystem::path dir = netProject("kast_replication_check");
    writeFile(dir / "certs/world_ca.pem", cert.certPem);
    writeFile(dir / kast::kProjectFileName,
              std::format(R"({{"version": 1, "name": "Replication", "startScene": null,
"online": {{"enabled": true, "authUrl": "http://127.0.0.1:{}", "caFile": "certs/world_ca.pem"}}}})",
                          authServer.port()));
    const std::filesystem::path source = KAST_TEST_PROJECT_DIR;
    std::filesystem::copy_file(source / "online_replication.scene", dir / "online_replication.scene");
    std::filesystem::copy_file(source / "scripts/online_replication_demo.lua", dir / "scripts/online_replication_demo.lua");
    // The scene a switch goes to: the same entities, a script that only logs despawns (the demo would log in again).
    {
        std::ifstream in(source / "online_replication.scene");
        Json switched = Json::parse(in);
        switched["systems"] = {"scripts/left.lua"};
        writeFile(dir / "switched.scene", switched.dump(2));
    }
    writeFile(dir / "scripts/left.lua", R"(return {
    init = function()
        kast.subscribe("world.despawn", function(e) kast.log("replication: server id " .. e.entityId .. " left") end)
    end,
    update = function(dt) end,
}
)");
    // A teleport from a script, 50 m in one frame: the server must reject it.
    writeFile(dir / "scripts/teleport.lua", R"(return {
    init = function()
        kast.subscribe("test.teleport", function()
            for e in kast.query("NetworkLocalPlayer") do
                local p = kast.get(e, "Transform").position
                kast.set(e, "Transform", {position = {x = p.x + 50, y = p.y, z = p.z}})
            end
        end)
        kast.subscribe("test.switch", function() kast.loadScene("switched.scene") end)
    end,
    update = function(dt) end,
}
)");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "online_replication.scene").string(), error) && doc.project,
          "replication: open the sample scene in an online project");
    doc.scene.systems.push_back("scripts/teleport.lua");
    const nlohmann::ordered_json before = kast::sceneToJson(doc.scene);
    startPlay(doc);
    kast::Scene& scene = doc.scene;
    check(named(scene, "Remote Player") == entt::null && named(scene, "Remote Body") == entt::null,
          "replication: the template is out of the Play scene (never rendered or simulated)");
    check(playUntil(doc, "replication: entered, player at 100, 0, 100"), "replication: world.entered places the player");
    check(playUntil(doc, "replication: Other spawned as entity"), "replication: world.spawn -> a clone + world.entitySpawned");
    const entt::entity clone = doc.lua.replication().clone(2);
    const auto& reg = scene.registry();
    check(scene.valid(clone) && reg.get<kast::NetworkEntity>(clone).entityId == 2 && countNamed(scene, "Remote Body") == 1 &&
              logged(std::format("replication: Other spawned as entity {} (server id 2)",
                                 reg.get<kast::PersistentId>(clone).value)),
          "replication: the clone carries NetworkEntity 2, the template's child, and Lua gets its entity id");
    // Spawn (101) and EntityMove (102) came in one write: the clone glides to 102 over 100 ms of frames.
    std::vector<float> xs;
    for (int frame = 0; frame < 12; ++frame) {
        xs.push_back(reg.get<kast::Transform>(clone).position.x);
        updatePlay(doc, 0.016f);
    }
    xs.push_back(reg.get<kast::Transform>(clone).position.x);
    const auto between = std::ranges::count_if(xs, [](float x) { return x > 101.01f && x < 101.99f; });
    check(std::ranges::is_sorted(xs) && between >= 3 && approx(xs.back(), 102.0f),
          std::format("replication: smooth remote motion ({} in-between frames, ends at {})", between, xs.back()));

    // Idle: nothing sent.
    for (int frame = 0; frame < 60; ++frame) {
        updatePlay(doc, 0.016f);
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);
    check(moves(fakeWorld).empty(), std::format("replication: an idle player sends no Move ({} sent)", moves(fakeWorld).size()));

    // Walking 1 s of game time at 3 m/s: about 10 Moves at 10 Hz, the last at the final position.
    const entt::entity player = named(scene, "Player");
    for (int frame = 0; frame < 63; ++frame) { // 63 x 16 ms = 1.008 s
        scene.registry().get<kast::Transform>(player).position.x += 0.048f;
        updatePlay(doc, 0.016f);
    }
    for (int frame = 0; frame < 20; ++frame) // standing: one more Move with the final position, then nothing
        updatePlay(doc, 0.016f);
    const bx::Vec3 walked = reg.get<kast::Transform>(player).position;
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < end &&
           (moves(fakeWorld).empty() || !approx(moves(fakeWorld).back().pos.x, walked.x)))
        std::this_thread::sleep_for(5ms);
    const std::vector<proto::Move> sent = moves(fakeWorld);
    check(sent.size() >= 10 && sent.size() <= 12,
          std::format("replication: 1 s of walking at sendRateHz 10 sent {} Moves (want 10-12)", sent.size()));
    check(!sent.empty() && approx(sent.back().pos.x, walked.x) && approx(sent.back().pos.z, 100.0f) &&
              approx(sent.back().heading, 0.5f),
          "replication: the last Move carries the final Transform position and its yaw as heading");

    // A teleport from a script is rejected; the correction snaps the player back.
    doc.lua.bus().publish("test.teleport", Json::object());
    updatePlay(doc, 0.016f); // the teleport, and its Move
    check(approx(reg.get<kast::Transform>(player).position.x, walked.x + 50, 1e-3f), "replication: the script teleported");
    const auto corrections = std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
        return contains(line, "replication: corrected to");
    }); // one so far: the fake's Correction after EnterWorldOk
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline &&
           std::ranges::count_if(kast::log::lines(), [](const std::string& line) {
               return contains(line, "replication: corrected to");
           }) == corrections) {
        updatePlay(doc, 0.016f);
        std::this_thread::sleep_for(1ms);
    }
    check(approx(reg.get<kast::Transform>(player).position, walked, 1e-3f),
          std::format("replication: the player is snapped back to x {} (now {})", walked.x,
                      reg.get<kast::Transform>(player).position.x));

    // A scene switch: the new scene's player stands where the old one walked to, and the remote player is there.
    const size_t movesBefore = moves(fakeWorld).size();
    doc.lua.bus().publish("test.switch", Json::object());
    updatePlay(doc, 0.016f);
    const entt::entity switched = named(scene, "Player");
    check(scene.valid(switched) && approx(reg.get<kast::Transform>(switched).position, walked, 1e-3f) &&
              approx(kast::headingOf(reg.get<kast::Transform>(switched).rotation), 0.5f),
          "replication: after a kast.loadScene switch the local player keeps its position and heading");
    check(scene.valid(doc.lua.replication().clone(2)) && countNamed(scene, "Remote Body") == 1,
          "replication: after a kast.loadScene switch the remote player is cloned again");
    for (int frame = 0; frame < 20; ++frame) {
        updatePlay(doc, 0.016f);
        std::this_thread::sleep_for(1ms);
    }
    std::this_thread::sleep_for(100ms);
    check(moves(fakeWorld).size() == movesBefore,
          "replication: a scene switch sends no Move (the player is where the server knows it)");

    doc.lua.world().say("bye"); // the fake world despawns entity 2
    check(playUntil(doc, "replication: server id 2 left"), "replication: world.despawn arrives");
    check(doc.lua.replication().clone(2) == entt::null && countNamed(scene, "Remote Body") == 0 &&
              countNamed(scene, "Other") == 0,
          "replication: despawn removes the clone");
    for (const kast::LuaSystemStatus& s : doc.lua.status())
        check(s.ok, std::format("replication: {} runs without errors {}", s.path, s.error));
    stopPlay(doc);
    check(kast::sceneToJson(doc.scene) == before,
          "replication: Stop restores the pre-Play scene exactly (template back, no clones)");
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

// The verification token Mailpit received for email (waits up to 15 s), or "".
std::string mailpitToken(Network& probe, const std::string& mailpitUrl, const std::string& email)
{
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
    return token;
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
    const std::string token = mailpitToken(probe, mailpitUrl, email);
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

// A verified, logged-in account adopted into auth; false if any step failed (logged).
bool liveAccount(kast::AuthClient& auth, Network& probe, const std::string& authUrl, const std::string& mailpitUrl,
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
    auth.adopt({.accessToken = login->second["accessToken"],
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
    if (!liveAccount(a.auth, probe, authUrl, mailpitUrl, std::format("kast-world-a-{}@example.com", stamp), password, ip(1)) ||
        !liveAccount(b.auth, probe, authUrl, mailpitUrl, std::format("kast-world-b-{}@example.com", stamp), password, ip(2))) {
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
    while (spawn && spawn->value("kind", "") == "npc") // the stack's sample NPC patrols the spawn point
        spawn = a.next("world.spawn");
    check(spawn && spawn->value("name", "") == heroB && spawn->value("entityId", 0u) == entityB &&
              spawn->value("kind", "") == "player",
          "live world: B's arrival is world.spawn {entityId, name, kind player} on A");
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

// kast_self_check --live-replication <authUrl> <mailpitUrl> <caFile> <realmName>: two players (each a LuaSystems
// session on its own scene with a NetworkLocalPlayer, a "Remote" template and a NetworkReplication, no scripts) enter
// the services/world compose stack's world next to each other. Each sees the other cloned; B walks 2 s at 4 m/s and
// A's clone of B follows smoothly; B's 50 m teleport is corrected; B leaving removes the clone on A. 77 (skipped) when
// the stack isn't up; 0 pass, 1 fail.
int liveReplicationCheck(const std::string& authUrl, const std::string& mailpitUrl, const std::string& caFile,
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

    struct Player {
        kast::Scene scene;
        kast::LuaSystems lua;
        std::vector<std::pair<std::string, Json>> events;
        std::vector<kast::EventBus::Subscription> subs;
        entt::entity local = entt::null;

        explicit Player(const kast::AuthConfig& config)
        {
            local = scene.create("Player");
            scene.registry().emplace<kast::NetworkLocalPlayer>(local);
            const entt::entity templ = scene.create("Remote");
            scene.registry().emplace<kast::MeshRenderer>(templ);
            const entt::entity manager = scene.create("Replication");
            scene.registry().emplace<kast::NetworkReplication>(manager, kast::NetworkReplication{"Remote", 100.0f});
            lua.start(scene, std::nullopt, "", nullptr, {}, config);
            for (const char* name : {"auth.realms", "world.connected", "world.charCreated", "world.entered",
                                     "world.spawn", "world.despawn", "world.correction", "world.disconnected",
                                     "world.entitySpawned", "world.charError", "world.mapChanged"})
                subs.push_back(lua.bus().subscribe(name, [this, name](const Json& e) { events.emplace_back(name, e); }));
        }
        const Json* last(const std::string& name) const
        {
            for (auto it = events.rbegin(); it != events.rend(); ++it)
                if (it->first == name)
                    return &it->second;
            return nullptr;
        }
        bx::Vec3& position() { return scene.registry().get<kast::Transform>(local).position; }
    };
    const kast::AuthConfig config{authUrl, caPem};
    Player a(config), b(config);
    // A shows terrain: the test project's maps (the stack's MAP_ROOT), map set by the server.
    a.lua.terrain().start(std::filesystem::path(KAST_TEST_PROJECT_DIR));
    const entt::entity terrainA = a.scene.create("Terrain");
    a.scene.registry().emplace<kast::Terrain>(terrainA);
    const auto tilesOf = [&](int map) {
        size_t count = 0;
        for (auto [e, tile] : a.scene.registry().view<kast::TerrainTile>().each())
            count += tile.map == map ? 1 : 0;
        return a.scene.registry().view<kast::TerrainTile>().size() == count ? count : 0;
    };
    // Ticks both sessions on a ~60 fps clock until done() (true) or timeout (false); step runs before each tick.
    auto previous = std::chrono::steady_clock::now();
    const auto run = [&](const std::function<bool()>& done, std::chrono::milliseconds timeout,
                         const std::function<void(float)>& step = {}) {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::sleep_for(15ms);
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - previous).count();
            previous = now;
            if (step)
                step(dt);
            kast::tickPlay(a.scene, a.lua, dt);
            kast::tickPlay(b.scene, b.lua, dt);
        }
        return true;
    };

    const std::string password = "live-replication-password-1";
    const std::string stamp = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t n = std::stoull(stamp) / 1000;
    const auto ip = [&](int account) { return std::format("10.{}.{}.{}", 80 + account, (n >> 8) % 256, n % 256); };
    if (!liveAccount(a.lua.auth(), probe, authUrl, mailpitUrl, std::format("kast-repl-a-{}@example.com", stamp),
                     password, ip(1)) ||
        !liveAccount(b.lua.auth(), probe, authUrl, mailpitUrl, std::format("kast-repl-b-{}@example.com", stamp),
                     password, ip(2))) {
        check(false, "live replication: two verified accounts");
        return 1;
    }
    a.lua.auth().realms();
    uint64_t realm = 0;
    run([&] {
        if (const Json* list = a.last("auth.realms"))
            for (const Json& r : list->at("realms"))
                if (r.value("name", "") == realmName)
                    realm = r.value("id", uint64_t{0});
        return realm != 0;
    }, 5s);
    if (!realm) {
        kast::log::warn(std::format("SKIPPED: no realm named '{}' (the ctest setup step adds it)", realmName));
        return kSkipped;
    }

    // Both enter the world. Every later step needs both players in it: a failed step ends the check.
    const auto reach = [&](Player& p, const char* event, const std::string& what) {
        const bool ok = run([&] { return p.last(event) || p.last("world.charError"); }, 10s) && p.last(event);
        check(ok, what);
        if (const Json* error = p.last("world.charError"); !ok && error)
            kast::log::error(std::format("{}: world.charError {}", what, error->dump()));
        return ok;
    };
    for (Player* p : {&a, &b}) {
        p->lua.world().connect(realm);
        if (!reach(*p, "world.connected", "live replication: connected"))
            return 1;
        p->lua.world().createCharacter(uniqueName(p == &a ? "Ra" : "Rb"));
        if (!reach(*p, "world.charCreated", "live replication: character created"))
            return 1;
        p->lua.world().enterWorld(p->last("world.charCreated")->at("character").value("id", uint64_t{0}));
        if (!reach(*p, "world.entered", "live replication: entered the world"))
            return 1;
    }
    const uint32_t idA = a.last("world.entered")->value("entityId", 0u);
    const uint32_t idB = b.last("world.entered")->value("entityId", 0u);
    check(run([&] { return a.scene.valid(a.lua.replication().clone(idB)) && b.scene.valid(b.lua.replication().clone(idA)); }, 10s),
          "live replication: each player sees the other as a clone");
    const bx::Vec3 startB = b.position();
    {
        const bx::Vec3 seen = a.scene.registry().get<kast::Transform>(a.lua.replication().clone(idB)).position;
        const bx::Vec3 seenA = b.scene.registry().get<kast::Transform>(b.lua.replication().clone(idA)).position;
        kast::log::info(std::format("live replication: A sees B (server id {}) at {:.2f}, {:.2f}, {:.2f}; B is at "
                                    "{:.2f}, {:.2f}, {:.2f}", idB, seen.x, seen.y, seen.z, startB.x, startB.y, startB.z));
        kast::log::info(std::format("live replication: B sees A (server id {}) at {:.2f}, {:.2f}, {:.2f}", idA, seenA.x,
                                    seenA.y, seenA.z));
        check(approx(seen, startB, 0.01f) && approx(seenA, a.position(), 0.01f),
              "live replication: clones spawn at the other player's position");
    }
    // Published with the clone, delivered next frame. The stack's sample Guard NPC spawns on A too, in any order.
    const auto spawnedB = [&] {
        return std::ranges::any_of(a.events, [&](const auto& e) {
            return e.first == "world.entitySpawned" && e.second.value("entityId", 0u) == idB;
        });
    };
    run(spawnedB, 1s);
    check(spawnedB(), "live replication: world.entitySpawned on A names B's server id");

    // B walks +x at 4 m/s for 2 s; A's clone of B is sampled every frame.
    std::vector<float> trail;
    float walked = 0;
    const entt::entity cloneB = a.lua.replication().clone(idB);
    run([&] { return walked >= 2.0f; }, 5s, [&](float dt) {
        b.position().x += 4.0f * dt;
        walked += dt;
        trail.push_back(a.scene.registry().get<kast::Transform>(cloneB).position.x);
    });
    run([] { return false; }, 600ms); // settle: the last Move, then 100 ms of interpolation
    const float finalB = b.position().x;
    const float finalSeen = a.scene.registry().get<kast::Transform>(cloneB).position.x;
    int still = 0;
    float biggest = 0;
    // Past the first update's latency, the clone should move on (nearly) every frame: 20 Hz or 10 Hz stepping would
    // leave runs of frames standing still and then jump.
    const size_t from = trail.size() / 4;
    for (size_t i = from + 1; i < trail.size(); ++i) {
        const float step = trail[i] - trail[i - 1];
        still += step < 1e-4f;
        biggest = std::max(biggest, step);
    }
    const int frames = static_cast<int>(trail.size() - from - 1);
    kast::log::info(std::format("live replication: B walked to x {:.2f}; A shows B at x {:.2f}; {} frames, {} standing "
                                "still, largest step {:.3f} m", finalB, finalSeen, frames, still, biggest));
    check(std::abs(finalSeen - finalB) < 0.05f, "live replication: A's clone of B ends where B is");
    check(still * 10 <= frames && biggest < 0.25f,
          "live replication: B's clone moves smoothly on A (no 10/20 Hz stepping)");

    // Speedhack: B teleports 50 m; the server rejects it and B is snapped back.
    const size_t corrections = static_cast<size_t>(std::ranges::count_if(b.events, [](const auto& e) {
        return e.first == "world.correction";
    }));
    const bx::Vec3 before = b.position();
    b.position().x += 50.0f;
    const bool corrected = run([&] {
        return static_cast<size_t>(std::ranges::count_if(b.events, [](const auto& e) {
                   return e.first == "world.correction";
               })) > corrections;
    }, 5s);
    run([] { return false; }, 300ms); // the server may repeat it once per tick; nothing moves meanwhile
    kast::log::info(std::format("live replication: B teleported to x {:.2f}, corrected back to x {:.2f}",
                                before.x + 50.0f, b.position().x));
    check(corrected && approx(b.position(), before, 0.05f), "live replication: a speedhack move is corrected and B snapped back");
    check(approx(a.scene.registry().get<kast::Transform>(cloneB).position, before, 0.05f),
          "live replication: A never saw the rejected move");

    b.lua.world().disconnect();
    check(run([&] { return a.lua.replication().clone(idB) == entt::null; }, 10s) && !a.scene.valid(cloneB),
          "live replication: B leaving despawns its clone on A");
    // Terrain: A entered on map 1 (start) and shows its 3 x 3 tiles; walking into start's hills_gate portal moves A to
    // map 3 (hills), and A's terrain follows.
    check(a.last("world.entered")->value("map", 0) == 1 && a.scene.registry().get<kast::Terrain>(terrainA).map == 1,
          "live replication: entering puts the local player on map 1 (Terrain.map 1)");
    check(run([&] { return tilesOf(1) == 9; }, 2s), "live replication: map 1's 3 x 3 tiles stream in around A");
    const bx::Vec3 gate{1065.0f, a.position().y, 1025.0f};
    run([&] { return a.last("world.mapChanged") != nullptr; }, 20s, [&](float dt) {
        const bx::Vec3 to = bx::sub(gate, a.position());
        const float left = bx::length(to);
        if (left > 1e-3f)
            a.position() = bx::add(a.position(), bx::mul(to, std::min(1.0f, 4.0f * dt / left))); // 4 m/s
    });
    const Json* changed = a.last("world.mapChanged");
    kast::log::info(std::format("live replication: A walked to {:.1f}, {:.1f}; mapChanged {}", a.position().x,
                                a.position().z, changed ? changed->dump() : "none"));
    check(changed && changed->value("map", 0) == 3, "live replication: walking into hills_gate moves A to map 3");
    check(a.scene.registry().get<kast::Terrain>(terrainA).map == 3,
          "live replication: world.mapChanged switches Terrain.map to the destination");
    check(run([&] { return tilesOf(3) == 4; }, 2s),
          "live replication: the hills' 4 tiles replace map 1's (no map 1 tile left)");
    kast::log::info(std::format("live replication: A on map 3 at {:.1f}, {:.1f}, {:.1f} with {} terrain tiles",
                                a.position().x, a.position().y, a.position().z,
                                a.scene.registry().view<kast::TerrainTile>().size()));

    // Ground (docs/services/world/maps.html#ground): the server put A on the ground its own heightmaps describe, the same
    // as A's terrain tiles; walking down the slope at that ground is never corrected. The walk stays under the hills'
    // nav.maxSlopeDeg (40; at most 36.5 here): steeper ground is off the server's navmesh ("Navmesh").
    std::shared_ptr<const kast::map::MapInfo> hills;
    kast::map::TileHeights hillsGround;
    for (auto [e, tile] : a.scene.registry().view<kast::TerrainTile>().each())
        if (tile.data) {
            hills = tile.data->map;
            hillsGround[{tile.tx, tile.tz}] = tile.data->heights;
        }
    const auto groundAt = [&](const bx::Vec3& p) {
        return hills ? kast::map::ground_at(*hills, hillsGround, p.x, p.z).value_or(-1e9f) : -1e9f;
    };
    const bx::Vec3 arrived = a.position();
    check(std::abs(arrived.y - groundAt(arrived)) < 0.05f,
          "live replication: the portal lands A on the ground of A's own terrain (server and client agree)");
    const auto correctionsOf = [](const Player& p) {
        return std::ranges::count_if(p.events, [](const auto& e) { return e.first == "world.correction"; });
    };
    const auto correctionsBefore = correctionsOf(a);
    const bx::Vec3 goal{arrived.x - 24.0f, 0.0f, arrived.z + 18.0f};
    run([&] { return std::hypot(goal.x - a.position().x, goal.z - a.position().z) < 1e-3f; }, 15s, [&](float dt) {
        const float dx = goal.x - a.position().x, dz = goal.z - a.position().z, left = std::hypot(dx, dz);
        const float k = std::min(1.0f, 4.0f * dt / left); // 4 m/s
        a.position().x += dx * k;
        a.position().z += dz * k;
        a.position().y = groundAt(a.position());
    });
    run([] { return false; }, 500ms); // the server's answer to the last moves
    kast::log::info(std::format("live replication: A walked the hills from y {:.2f} to {:.2f} at ({:.1f}, {:.1f})",
                                arrived.y, a.position().y, a.position().x, a.position().z));
    check(correctionsOf(a) == correctionsBefore && std::abs(a.position().y - arrived.y) > 1.0f,
          "live replication: walking 30 m of the hills at the ground's height is never corrected");

    a.lua.world().disconnect();
    a.lua.stop();
    b.lua.stop();
    if (g_failures == 0)
        kast::log::info("live replication check passed");
    return g_failures == 0 ? 0 : 1;
}

namespace {

// --- the editor's Online panel model (editor/online.h), against FakeAuth and the fake world server ---

// Polls online (for project) until done() or timeout; false on timeout.
bool pollUntil(OnlineModel& online, const kast::Project* project, const std::function<bool()>& done,
               std::chrono::milliseconds timeout = 5s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        online.poll(project, false);
        if (done())
            return true;
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
}

std::string readAll(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void checkOnlineHelpers()
{
    using State = OnlineProbe::State;
    check(contains(composeBlocker(false, "C:/anything"), "docker was not found on PATH"), "online: no docker -> reason");
    check(contains(composeBlocker(true, ""), "File > Preferences"), "online: no compose folder -> set it in Preferences");
    const std::filesystem::path dir = netRoot() / "kast_compose_check";
    std::filesystem::create_directories(dir);
    check(contains(composeBlocker(true, (dir / "missing").string()), "does not exist"), "online: a missing folder -> reason");
    check(contains(composeBlocker(true, dir.string()), "has no docker-compose.yml"), "online: no compose file -> reason");
    writeFile(dir / "docker-compose.yml", "services: {}\n");
    check(composeBlocker(true, dir.string()).empty(), "online: docker + a folder with docker-compose.yml -> can run");
    const std::filesystem::path world = std::filesystem::path(KAST_SOURCE_DIR) / "services/world";
    check(std::filesystem::path(defaultComposeFolder()) == world.lexically_normal(),
          "online: the default compose folder is services/world of the source tree");

    const std::vector<ComposeService> lines = parseComposePs(
        "{\"Service\":\"world\",\"State\":\"running\",\"Health\":\"healthy\"}\n"
        "not json\n{\"Service\":\"auth\",\"State\":\"exited\",\"Health\":\"\"}\n");
    check(lines.size() == 2 && lines[0].name == "auth" && lines[0].state == "exited" && lines[1].health == "healthy",
          "online: compose ps as one object per line, sorted by service, junk skipped");
    const std::vector<ComposeService> array =
        parseComposePs("[{\"Service\":\"mysql\",\"State\":\"running\"},{\"Name\":\"no service\"}]");
    check(array.size() == 1 && array[0].name == "mysql" && array[0].health.empty(), "online: compose ps as a JSON array");
    check(OnlineProbe{}.state == State::Unknown, "online: probes start unknown");
}

void checkOnlineModel()
{
    using State = OnlineProbe::State;
    checkOnlineHelpers();
    const std::filesystem::path settings = netRoot() / "kast_online_settings/editor_settings.json";
    std::string error;
    check(saveEditorCommand(settings, "notepad {file}", error), "online: an existing externalEditor setting");

    // Disabled: no checks, no login.
    const std::filesystem::path offDir = netProject("kast_online_off_check");
    kast::Project off;
    check(kast::loadProject(offDir / kast::kProjectFileName, off, error), "online: load the disabled project");
    {
        OnlineModel online(settings, "");
        online.poll(&off, true);
        std::string password = "x-password";
        online.login("someone@test", password);
        check(!online.enabled() && online.authHealth().state == State::Unknown && online.loginError().code == "disabled" &&
                  password.empty(),
              "online: disabled project -> no health check, login refused inline, password wiped");
        check(contains(online.composeBlocker(), "Preferences") || contains(online.composeBlocker(), "docker"),
              "online: no compose folder -> Start/Stop/Status blocked with a reason");
    }

    // Auth down.
    const std::filesystem::path downDir = netProject("kast_online_down_check");
    writeFile(downDir / kast::kProjectFileName, R"({"version": 1, "name": "Down", "startScene": null,
"online": {"enabled": true, "authUrl": "http://127.0.0.1:1"}})");
    kast::Project down;
    check(kast::loadProject(downDir / kast::kProjectFileName, down, error), "online: load the auth-down project");
    {
        OnlineModel online(settings, "");
        online.poll(&down, true); // visible: the first refresh starts at once
        check(online.authHealth().state == State::Checking, "online: a visible panel checks /health at once");
        check(pollUntil(online, &down, [&] { return online.authHealth().state == State::Down; }),
              "online: auth not running -> unreachable");
    }

    FakeAuth fake;
    Server authServer(serveFakeAuth(fake));
    const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1");
    const Cert stranger = makeCert("DNS:localhost,IP:127.0.0.1"); // a world server whose certificate isn't the CA file's
    FakeWorld fakeWorld, untrustedWorld;
    Server worldServer(serveFakeWorld(fakeWorld), 0, &cert);
    Server untrustedServer(serveFakeWorld(untrustedWorld), 0, &stranger);
    fake.worldPort = worldServer.port();
    const std::filesystem::path dir = netProject("kast_online_check");
    writeFile(dir / "certs/world_ca.pem", cert.certPem);
    writeFile(dir / kast::kProjectFileName,
              std::format(R"({{"version": 1, "name": "Online", "startScene": null,
"online": {{"enabled": true, "authUrl": "http://127.0.0.1:{}", "caFile": "certs/world_ca.pem"}}}})",
                          authServer.port()));
    saveScene(dir / "a.scene", {"scripts/session.lua"});
    writeFile(dir / "scripts/session.lua", R"(local reported = false
return {
    init = function()
        kast.subscribe("auth.session", function(e) kast.log("online check auth.session " .. e.email .. " " .. e.role) end)
    end,
    update = function(dt)
        if reported then return end
        reported = true
        local account = kast.auth.account()
        kast.log("online check first update " .. (account and account.email or "logged out"))
    end,
}
)");
    Document doc;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "online: open the online project");
    const kast::Project* project = &*doc.project;
    OnlineModel online(settings, "");
    doc.onPlayStarted = [&online](kast::LuaSystems& lua) { online.lendSession(lua.auth()); };
    doc.onPlayStopping = [&online](kast::LuaSystems& lua) { online.reclaimSession(lua.auth()); };

    online.poll(project, false);
    check(online.enabled() && online.authHealth().state == State::Unknown, "online: a hidden panel checks nothing");
    online.refresh();
    check(pollUntil(online, project, [&] { return online.authHealth().state == State::Ok; }), "online: /health ok");
    fake.dbDown = true;
    online.refresh();
    check(pollUntil(online, project, [&] { return online.authHealth().state == State::Warning; }) &&
              contains(online.authHealth().detail, "db_unavailable"),
          "online: /health 503 -> database down (db_unavailable)");
    fake.dbDown = false;
    check(online.realms().empty(), "online: no realms while logged out");

    std::string password = "wrong-password";
    online.login(kDemoEmail, password);
    check(password.empty() && online.loggingIn(), "online: Log in wipes the password field at once");
    check(pollUntil(online, project, [&] { return !online.loggingIn(); }) &&
              online.loginError().code == "invalid_credentials" && !online.loggedIn(),
          "online: a wrong password -> invalid_credentials inline");
    password = kDemoPassword;
    online.login(kDemoEmail, password);
    check(pollUntil(online, project, [&] { return online.loggedIn(); }) && online.loginError().code.empty() &&
              online.account().value("email", "") == kDemoEmail && online.account().value("role", "") == "player" &&
              !online.isAdmin(),
          "online: login shows email and role; a player is no admin");
    check(pollUntil(online, project, [&] {
              return online.realms().size() == 3 && std::ranges::none_of(online.realms(), [](const OnlineRealm& r) {
                         return r.world.state == State::Checking || r.world.state == State::Unknown;
                     });
          }),
          "online: logged in -> the realm list, each world server checked");
    if (online.realms().size() == 3) {
        check(online.realms()[0].name == "Ashenvale" && online.realms()[0].world.state == State::Down,
              "online: a realm with nothing listening -> world unreachable");
        check(online.realms()[2].name == "Testgrove" && online.realms()[2].world.state == State::Ok,
              "online: a world server trusted through the project's CA file -> reachable");
    }
    online.createRealm({{"name", "Nope"}, {"host", "127.0.0.1"}, {"port", 1}, {"region", "x"}});
    check(online.adminResult().error.code == "not_admin" && !online.adminResult().busy,
          "online: admin calls refused without an admin login");

    // Play hand-off.
    startPlay(doc);
    check(online.sessionLent() && !online.loggedIn(), "online: Play takes the login; the editor holds none");
    check(doc.lua.auth().loggedIn() && doc.lua.auth().accessToken() == fake.accessToken(),
          "online: the Play session adopted the editor's tokens");
    check(playUntil(doc, std::format("online check auth.session {} player", kDemoEmail)) &&
              playUntil(doc, std::format("online check first update {}", kDemoEmail)),
          "online: Lua gets auth.session and is logged in from the first update");
    password = kDemoPassword;
    online.login(kDemoEmail, password);
    check(online.loginError().code == "playing" && password.empty(), "online: no login during Play");
    stopPlay(doc);
    check(!online.sessionLent() && online.loggedIn() && online.account().value("email", "") == kDemoEmail,
          "online: Stop gives the login back to the editor");
    const int refreshes = fake.refreshes();
    startPlay(doc);
    check(playUntil(doc, "online check auth.session"), "online: a second Play gets the session again");
    doc.lua.auth().logout();
    stopPlay(doc);
    check(!online.loggedIn() && !online.sessionLent() && logged("Play ended logged out"),
          "online: Lua logging out in Play logs the editor out too");
    check(fake.refreshes() == refreshes, "online: the hand-off itself refreshes nothing");
    startPlay(doc);
    check(playUntil(doc, "online check first update logged out"), "online: not logged in -> Play starts logged out");
    stopPlay(doc);

    // Admin.
    password = kDemoPassword;
    online.login(kAdminEmail, password);
    check(pollUntil(online, project, [&] { return online.isAdmin(); }), "online: an admin login");
    check(pollUntil(online, project, [&] { return online.realms().size() == 3; }), "online: the admin sees the realms");
    const auto admin = [&](const std::function<void()>& call) {
        call();
        return pollUntil(online, project, [&] { return !online.adminResult().busy; });
    };
    const auto failedWith = [&](const char* code) {
        return !online.adminResult().ok && online.adminResult().error.code == code;
    };
    const Json realm{{"name", "Untrusted"}, {"host", "127.0.0.1"}, {"port", untrustedServer.port()},
                     {"region", "local"}, {"status", "online"}, {"sortOrder", 5}};
    check(admin([&] { online.createRealm({{"name", "Ashenvale"}, {"host", "h"}, {"port", 1}, {"region", "r"}}); }) &&
              failedWith("realm_name_taken"),
          "online: create with a taken name -> realm_name_taken inline");
    check(admin([&] { online.createRealm(realm); }) && online.adminResult().ok && online.adminResult().op == "createRealm",
          "online: create a realm");
    check(pollUntil(online, project, [&] {
              const auto it = std::ranges::find(online.realms(), "Untrusted", &OnlineRealm::name);
              return it != online.realms().end() && it->world.state == State::Warning &&
                     contains(it->world.detail, "not trusted");
          }),
          "online: the new realm listed; its world server's certificate isn't trusted -> warning");
    const auto created = std::ranges::find(online.realms(), "Untrusted", &OnlineRealm::name);
    const int64_t id = created == online.realms().end() ? 0 : created->id;
    Json renamed = realm;
    renamed["name"] = "Untrusted 2";
    check(admin([&] { online.updateRealm(id, renamed); }) && online.adminResult().ok, "online: edit a realm");
    check(pollUntil(online, project, [&] { return std::ranges::any_of(online.realms(), [](const OnlineRealm& r) { return r.name == "Untrusted 2"; }); }),
          "online: the edit shows in the list");
    check(admin([&] { online.deleteRealm(id); }) && online.adminResult().ok, "online: delete a realm");
    check(pollUntil(online, project, [&] { return online.realms().size() == 3; }), "online: the deleted realm is gone");
    check(admin([&] { online.deleteRealm(999); }) && failedWith("realm_not_found"), "online: delete unknown -> realm_not_found");
    check(admin([&] { online.ban(1, "self", ""); }) && failedWith("cannot_modify_self"),
          "online: banning yourself -> cannot_modify_self inline");
    check(admin([&] { online.ban(5, "", ""); }) && failedWith("validation_error"), "online: ban without a reason -> validation_error");
    check(admin([&] { online.ban(5, "cheating", "2030-01-01T00:00:00Z"); }) && online.adminResult().ok, "online: ban");
    check(admin([&] { online.unban(5); }) && online.adminResult().ok, "online: unban");
    check(admin([&] { online.setRole(1, "player"); }) && failedWith("cannot_modify_self"),
          "online: changing your own role -> cannot_modify_self");
    check(admin([&] { online.setRole(2000, "gm"); }) && failedWith("account_not_found"), "online: unknown account");
    check(admin([&] { online.setRole(5, "gm"); }) && online.adminResult().ok, "online: set role");
    const std::vector<FakeAuth::Seen> seen = fake.seen();
    const auto adminCall = [](const FakeAuth::Seen& s) { return s.path.starts_with("/v1/admin/"); };
    check(std::ranges::count_if(seen, adminCall) >= 10 &&
              std::ranges::all_of(seen | std::views::filter(adminCall),
                                  [](const FakeAuth::Seen& s) { return s.authorization.starts_with("Bearer "); }),
          "online: admin calls carry the admin's Bearer token");
    check(std::ranges::any_of(seen, [](const FakeAuth::Seen& s) {
              return s.path == "/v1/admin/accounts/5/ban" && s.method == "POST" && s.body.is_object() &&
                     s.body.contains("expiresAt") && s.body["expiresAt"] == "2030-01-01T00:00:00Z";
          }),
          "online: ban sends reason and expiresAt");

    online.logout();
    check(!online.loggedIn() && online.realms().empty(), "online: Log out");
    check(pollUntil(online, project, [&] { return fake.seen().back().path == "/v1/auth/logout"; }),
          "online: Log out revokes the session on the server");

    // Settings: the email, compose folder and override persist; no secret does.
    check(!online.setWorldOverride("nonsense", error) && online.worldOverride().empty(), "online: a bad override is refused");
    check(online.setWorldOverride("127.0.0.1:7777", error) && online.setComposeFolder("C:/stack", error),
          "online: save the override and compose folder");
    const std::string saved = readAll(settings);
    check(contains(saved, std::format("\"onlineEmail\": \"{}\"", kAdminEmail)) &&
              contains(saved, "\"worldOverride\": \"127.0.0.1:7777\"") && contains(saved, "\"composeFolder\": \"C:/stack\"") &&
              contains(saved, "\"externalEditor\": \"notepad {file}\""),
          "online: email, override and compose folder saved beside the external editor command");
    check(!contains(saved, kDemoPassword) && !contains(saved, "eyJ") && !contains(saved, "refresh-") &&
              !contains(saved, "Token"),
          "online: no password or token in the settings file");
    {
        OnlineModel again(settings, "C:/default");
        check(again.email() == kAdminEmail && again.worldOverride() == "127.0.0.1:7777" && again.composeFolder() == "C:/stack",
              "online: a new editor run remembers email, override and compose folder");
    }
    std::string logText;
    for (const std::string& line : kast::log::lines())
        logText += line + "\n";
    check(!contains(logText, kDemoPassword) && !contains(logText, "wrong-password") && !contains(logText, "eyJ") &&
              !contains(logText, "refresh-"),
          "online: no password or token in the log");
}

} // namespace

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
    checkWorldSendOffline();
    checkReplicationOffline();
    checkReplicationPrefabTemplate();
    checkReplicationNpcs();
    checkReplicationComponents();
    checkReplicationFakeServer();
    checkAuthLogs();
    checkOnlineModel();
    std::error_code ec;
    std::filesystem::remove_all(netRoot(), ec);
    return g_failures;
}

// kast_self_check --live-online <authUrl> <mailpitUrl> <composeFolder>: the editor's Online panel model against the
// services/world compose stack. Starts it through the model's Start (docker compose up -d --build) unless it is already
// running; /health ok; a fresh account verified through Mailpit and made admin in auth's database (test data only);
// login; realm create, list and delete; a Play session started logged in (auth.session reaches Lua); then no password
// or token in the editor settings, the project or a built game.pak. Stops the stack again (docker compose down) only if
// it started it. 77 (skipped) without docker, without a running Docker engine or when registration is rate limited.
int liveOnlineCheck(const std::string& authUrl, const std::string& mailpitUrl, const std::string& composeFolder)
{
    constexpr int kSkipped = 77;
    using State = OnlineProbe::State;
    const std::filesystem::path settings = netRoot() / "kast_online_live/editor_settings.json";
    const std::filesystem::path dir = netProject("kast_online_live_project");
    writeFile(dir / kast::kProjectFileName,
              std::format(R"({{"version": 1, "name": "Online Live", "startScene": "a.scene",
"online": {{"enabled": true, "authUrl": "{}"}}}})",
                          authUrl));
    saveScene(dir / "a.scene", {"scripts/session.lua"});
    writeFile(dir / "scripts/session.lua", R"(return {
    init = function()
        kast.subscribe("auth.session", function(e) kast.log("live online auth.session " .. e.email .. " " .. e.role) end)
    end,
    update = function(dt) end,
}
)");
    Document doc;
    std::string error;
    check(openFile(doc, (dir / "a.scene").string(), error) && doc.project, "live online: open the project");
    if (!doc.project)
        return 1;
    const kast::Project* project = &*doc.project;
    OnlineModel online(settings, composeFolder);
    doc.onPlayStarted = [&online](kast::LuaSystems& lua) { online.lendSession(lua.auth()); };
    doc.onPlayStopping = [&online](kast::LuaSystems& lua) { online.reclaimSession(lua.auth()); };
    online.poll(project, false);
    if (!online.composeBlocker().empty()) {
        kast::log::warn(std::format("SKIPPED: {}", online.composeBlocker()));
        return kSkipped;
    }
    const auto compose = [&](OnlineModel::Compose what, std::chrono::milliseconds timeout) {
        check(online.runCompose(what), std::format("live online: start {}", static_cast<int>(what)));
        // Up and Down chain a Ps: wait for that one too.
        return pollUntil(online, project, [&] { return !online.composeBusy(); }, timeout) &&
               (what == OnlineModel::Compose::Ps || pollUntil(online, project, [&] { return !online.composeBusy(); }, 60s));
    };
    const auto running = [&] {
        return online.services().size() >= 4 &&
               std::ranges::all_of(online.services(), [](const ComposeService& s) { return s.state == "running"; });
    };
    if (!compose(OnlineModel::Compose::Ps, 60s) || !online.composeError().empty()) {
        kast::log::warn(std::format("SKIPPED: docker compose ps failed ({}); is Docker Desktop running?", online.composeError()));
        return kSkipped;
    }
    const bool wasUp = running();
    kast::log::info(wasUp ? "live online: the stack is already running" : "live online: starting the stack");
    if (!wasUp) {
        check(compose(OnlineModel::Compose::Up, 900s) && online.composeError().empty(),
              "live online: Start (docker compose up -d --build) succeeds");
        check(running(), "live online: every service is running after Start");
    }
    const auto healthy = [&] {
        return pollUntil(online, project, [&] {
            if (online.authHealth().state == State::Ok)
                return true;
            if (online.authHealth().state != State::Checking)
                online.refresh();
            return false;
        }, 120s);
    };
    check(healthy(), "live online: auth /health ok");

    // A fresh account, verified, then made admin straight in auth's database: the stack seeds no admin.
    const std::string email = std::format("kast-editor-{}@example.com",
                                          std::chrono::system_clock::now().time_since_epoch().count());
    const std::string password = "live-online-password-1";
    {
        AuthRun r(authUrl);
        const uint64_t reg = r.auth.registerAccount(email, password);
        if (r.failed(reg, "register", "rate_limited")) {
            kast::log::warn("SKIPPED: registration is rate limited (5/hour/IP); try again later");
            return kSkipped;
        }
        Network probe;
        const std::string token = mailpitToken(probe, mailpitUrl, email);
        check(!token.empty() && r.succeeded(r.auth.verifyEmail(token), "auth.emailVerified"), "live online: verify the account");
    }
    const std::string sql = std::format("mysql -uroot -p\"$MYSQL_ROOT_PASSWORD\" \"$MYSQL_DATABASE\" -e \"UPDATE accounts "
                                        "SET role='admin' WHERE email='{}'\"",
                                        email);
    const std::string file = (std::filesystem::path(composeFolder) / "docker-compose.yml").string();
    const char* args[] = {"docker", "compose", "-f", file.c_str(), "exec", "-T", "mysql", "sh", "-c", sql.c_str(), nullptr};
    SDL_Process* promote = SDL_CreateProcess(args, false);
    int code = -1;
    check(promote && SDL_WaitProcess(promote, true, &code) && code == 0, "live online: make the account admin (test data)");
    if (promote)
        SDL_DestroyProcess(promote);

    std::string typed = password;
    online.login(email, typed);
    check(pollUntil(online, project, [&] { return !online.loggingIn(); }, 10s) && online.isAdmin() &&
              online.account().value("email", "") == email,
          "live online: Log in shows the account as admin");
    check(pollUntil(online, project, [&] { return !online.realms().empty(); }, 10s), "live online: the realm list");

    const std::string realmName = std::format("Editor Live {}", std::chrono::system_clock::now().time_since_epoch().count() % 1000000);
    online.createRealm({{"name", realmName}, {"host", "127.0.0.1"}, {"port", 7777}, {"region", "local"}, {"status", "offline"}});
    check(pollUntil(online, project, [&] { return !online.adminResult().busy; }, 10s) && online.adminResult().ok,
          "live online: create a realm");
    const auto find = [&] { return std::ranges::find(online.realms(), realmName, &OnlineRealm::name); };
    check(pollUntil(online, project, [&] { return find() != online.realms().end(); }, 10s), "live online: the new realm is listed");
    if (find() != online.realms().end()) {
        const int64_t id = find()->id;
        online.createRealm({{"name", realmName}, {"host", "127.0.0.1"}, {"port", 7777}, {"region", "local"}});
        check(pollUntil(online, project, [&] { return !online.adminResult().busy; }, 10s) &&
                  online.adminResult().error.code == "realm_name_taken",
              "live online: a taken name -> realm_name_taken");
        online.deleteRealm(id);
        check(pollUntil(online, project, [&] { return !online.adminResult().busy; }, 10s) && online.adminResult().ok,
              "live online: delete the realm");
        check(pollUntil(online, project, [&] { return find() == online.realms().end(); }, 10s), "live online: the realm is gone");
    }
    online.ban(0, "x", ""); // id 0: the service's own validation, nothing banned
    check(pollUntil(online, project, [&] { return !online.adminResult().busy; }, 10s) && !online.adminResult().ok &&
              !online.adminResult().error.code.empty(),
          "live online: an invalid ban shows the service's error code");
    const int64_t self = online.account().value("accountId", int64_t{0});
    online.setRole(self, "player");
    check(pollUntil(online, project, [&] { return !online.adminResult().busy; }, 10s) &&
              online.adminResult().error.code == "cannot_modify_self",
          "live online: changing your own role -> cannot_modify_self");

    startPlay(doc);
    check(playUntil(doc, std::format("live online auth.session {} admin", email), 10s),
          "live online: Play starts logged in, auth.session reaches Lua");
    const std::optional<kast::AuthSession> tokens = doc.lua.auth().session();
    stopPlay(doc);
    check(tokens && online.loggedIn(), "live online: Stop gives the login back");

    // No secret anywhere on disk: the settings, the project and a built game.pak.
    std::filesystem::path exe;
    const std::filesystem::path out = netRoot() / "kast_online_live_build";
    const char* base = SDL_GetBasePath();
    check(base && buildGame(*doc.project, out, std::filesystem::path(reinterpret_cast<const char8_t*>(base)), exe, error),
          std::format("live online: build the game ({})", error));
    std::vector<std::filesystem::path> files{settings, out / kGamePakName};
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir))
        if (entry.is_regular_file())
            files.push_back(entry.path());
    std::vector<std::string> secrets{password};
    if (tokens)
        secrets.insert(secrets.end(), {tokens->accessToken, tokens->refreshToken});
    for (const std::filesystem::path& f : files) {
        const std::string text = readAll(f);
        for (const std::string& secret : secrets)
            check(!contains(text, secret), std::format("live online: no password or token in {}", f.string()));
    }
    check(contains(readAll(settings), email), "live online: the email is remembered");
    std::string logText;
    for (const std::string& line : kast::log::lines())
        logText += line + "\n";
    for (const std::string& secret : secrets)
        check(!contains(logText, secret), "live online: no password or token in the log");

    online.logout();
    if (!wasUp) {
        check(compose(OnlineModel::Compose::Down, 300s) && online.composeError().empty(),
              "live online: Stop (docker compose down) succeeds");
        check(std::ranges::none_of(online.services(), [](const ComposeService& s) { return s.state == "running"; }),
              "live online: nothing is running after Stop");
        online.refresh();
        check(pollUntil(online, project, [&] { return online.authHealth().state == State::Down; }, 20s),
              "live online: auth unreachable after Stop");
    } else {
        kast::log::info("live online: the stack was already running, so it is left running");
    }
    if (g_failures == 0)
        kast::log::info("live online check passed");
    return g_failures == 0 ? 0 : 1;
}
