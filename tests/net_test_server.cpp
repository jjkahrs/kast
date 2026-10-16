// kast_net_test_server: loopback servers for the tests/project net scenes (net_a.scene -> net_b.scene), on fixed ports
// since Lua can't be told others: 47701 TCP, 47702 TLS TCP, 47703 ws, 47704 wss, 47705 http, 47706 https, 47707 a fake
// world server (FakeWorld in net_test_util.h, TLS; the fake auth's realm 3, Testgrove, points at it). TLS uses
// --cert/--key (tests/project/certs/net_test_ca.pem is the certificate, which net_a.lua passes as caFile).
//
// TCP / TLS: '\n'-terminated lines. "id" -> "id <n>" (n: this server's accept count when the connection came, so 1
// means nothing reconnected); "stream <n>" -> "tick 1" .. "tick <n>" in bursts, then "done <n>"; anything else is
// echoed. ws / wss: messages, the same commands. http / https: any request -> 200 JSON {method, target, body};
// "/slow" waits 300 ms first. A target holding "/v1/" goes to a fake auth service instead (FakeAuth in
// net_test_util.h: kast.auth scenes log in as demo@example.com / demo-password-123; "/stall/v1/..." answers after 3 s),
// and so does a target ending in "/health".
//
// Usage: kast_net_test_server --cert <pem> --key <pem> -- <exe> [args...]   runs exe with the servers up and exits with
//        its exit code (its output goes to ours)
//        kast_net_test_server --make-cert <cert.pem> <key.pem>              writes a new 100-year localhost certificate
#include "net_test_util.h"

#include <nlohmann/json.hpp>

#include <process.h>

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

using namespace kast::nettest;

std::string readText(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// One command line -> the reply lines (or messages).
template <class Send>
void answer(const std::string& line, int id, Send&& send)
{
    if (line == "id")
        return send(std::format("id {}", id));
    if (line.starts_with("stream ")) {
        const int n = std::stoi(line.substr(7));
        for (int i = 1; i <= n; ++i) {
            send(std::format("tick {}", i));
            if (i % 7 == 0) // bursts with gaps: some ticks land in the frame of the scene switch, some after it
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        return send(std::format("done {}", n));
    }
    send(line);
}

template <class Stream>
void lines(Stream stream, int id)
{
    std::string buffer;
    for (;;) {
        const std::string line = readLine(stream, buffer);
        answer(line, id, [&](const std::string& reply) { asio::write(stream, asio::buffer(reply + "\n")); });
    }
}

template <class Ws>
void messages(Ws ws, int id)
{
    ws.accept();
    for (;;) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        answer(beast::buffers_to_string(buffer.data()), id, [&](const std::string& reply) {
            ws.text(true);
            ws.write(asio::buffer(reply));
        });
    }
}

FakeAuth g_auth;
FakeWorld g_world;

template <class Stream>
void respond(Stream stream)
{
    beast::flat_buffer buffer;
    http::request<http::string_body> req;
    http::read(stream, buffer, req);
    if (req.target().find("/v1/") != std::string_view::npos || req.target().ends_with("/health"))
        return (void)http::write(stream, g_auth.answer(req));
    if (req.target().starts_with("/slow"))
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    http::response<http::string_body> res{http::status::ok, 11};
    res.set(http::field::content_type, "application/json");
    res.body() = nlohmann::json{{"method", req.method_string()}, {"target", req.target()}, {"body", req.body()}}.dump();
    res.prepare_payload();
    http::write(stream, res);
}

TlsSocket handshake(Server& server, tcp::socket socket)
{
    TlsSocket stream(std::move(socket), server.tls());
    stream.handshake(ssl::stream_base::server);
    return stream;
}

// Accepts forever; handle(server, socket, id) runs on a thread per connection.
template <class Handle>
Server::Body serve(Handle handle)
{
    return [handle](Server& server) {
        for (;;) {
            tcp::socket socket = server.accept();
            std::thread([&server, handle, socket = std::move(socket), id = server.accepted()]() mutable {
                try {
                    handle(server, std::move(socket), id);
                } catch (const std::exception&) { // the client went away
                }
            }).detach();
        }
    };
}

int run(const Cert& cert, std::vector<std::string> command)
{
    using Socket = tcp::socket;
    Server tcpServer(serve([](Server&, Socket socket, int id) { lines(std::move(socket), id); }), 47701);
    Server tlsServer(serve([](Server& s, Socket socket, int id) { lines(handshake(s, std::move(socket)), id); }), 47702,
                     &cert);
    Server wsServer(serve([](Server&, Socket socket, int id) {
                        messages(websocket::stream<tcp::socket>(std::move(socket)), id);
                    }),
                    47703);
    Server wssServer(serve([](Server& s, Socket socket, int id) {
                         messages(websocket::stream<TlsSocket>(handshake(s, std::move(socket))), id);
                     }),
                     47704, &cert);
    Server httpServer(serve([](Server&, Socket socket, int) { respond(std::move(socket)); }), 47705);
    Server httpsServer(serve([](Server& s, Socket socket, int) { respond(handshake(s, std::move(socket))); }), 47706,
                       &cert);
    Server worldServer(serveFakeWorld(g_world), 47707, &cert);
    std::vector<std::string> quoted;
    for (const std::string& arg : command)
        quoted.push_back('"' + arg + '"'); // ponytail: no embedded quotes in test arguments
    std::vector<const char*> argv;
    for (const std::string& arg : quoted)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    std::cout.flush();
    const intptr_t code = _spawnv(_P_WAIT, command[0].c_str(), argv.data());
    if (code == -1) {
        std::cerr << "kast_net_test_server: cannot run " << command[0] << "\n";
        return 1;
    }
    std::quick_exit(static_cast<int>(code)); // connection threads are detached; nothing to clean up
}

} // namespace

int main(int argc, char** argv)
{
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 3 && args[0] == "--make-cert") {
        const Cert cert = makeCert("DNS:localhost,IP:127.0.0.1", 36500);
        std::ofstream(args[1], std::ios::binary) << cert.certPem;
        std::ofstream(args[2], std::ios::binary) << cert.keyPem;
        return 0;
    }
    if (args.size() >= 6 && args[0] == "--cert" && args[2] == "--key" && args[4] == "--")
        return run({readText(args[1]), readText(args[3])}, {args.begin() + 5, args.end()});
    std::cerr << "Usage: kast_net_test_server --cert <pem> --key <pem> -- <exe> [args...]\n"
                 "       kast_net_test_server --make-cert <cert.pem> <key.pem>\n";
    return 2;
}
