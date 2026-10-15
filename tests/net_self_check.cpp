// kast::Network and kast.net checks for kast_self_check, against loopback servers (tests/net_test_util.h) on ephemeral
// ports with certificates made at run time. Kept out of self_check.cpp so only this file compiles Boost.Asio / Beast.
#include "net_test_util.h"

#include "document.h"
#include "log.h"
#include "lua_systems.h"
#include "network.h"
#include "project.h"
#include "scene_io.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <process.h>
#include <fstream>
#include <source_location>
#include <thread>

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
    std::error_code ec;
    std::filesystem::remove_all(netRoot(), ec);
    return g_failures;
}
