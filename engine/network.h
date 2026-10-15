#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Network: client TCP (plain or TLS), WebSocket (ws/wss) connections and one-shot HTTP/HTTPS requests on Boost.Asio +
// Beast + OpenSSL. One background thread runs every socket; results wait in a queue until poll() hands them to the
// owner's thread as plain JSON events (always valid UTF-8). No Lua, no EventBus: LuaSystems owns one per Play session,
// binds it as kast.net and publishes what poll() returns. Every public function must be called on the thread that
// constructed the Network (checked in debug builds). See README "Networking".
namespace kast {

enum class NetType { Tcp, Ws };
enum class NetEncoding { Text, Base64, Json };
enum class NetFraming { Raw, U16le, Line };
enum class NetState { Connecting, Open, Closed };

const char* netStateName(NetState state); // "connecting", "open", "closed"

constexpr size_t kMaxNetMessage = 16 * 1024 * 1024; // one message, frame, line or HTTP body
constexpr std::chrono::milliseconds kDefaultNetTimeout{10000};

struct NetOpenOptions {
    std::string name;
    NetType type = NetType::Tcp;
    std::string host;  // Tcp
    uint16_t port = 0; // Tcp
    bool tls = false;  // Tcp
    std::string url;   // Ws: ws://host[:port][/path] or wss://...
    std::string caPem; // extra trusted CA certificates (PEM text), besides the Windows ROOT store
    NetEncoding encoding = NetEncoding::Text;
    NetFraming framing = NetFraming::Raw; // Tcp
    std::chrono::milliseconds timeout = kDefaultNetTimeout; // resolve + connect + handshakes
};

struct NetRequestOptions {
    std::string method = "GET";
    std::string url; // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string caPem;
    NetEncoding encoding = NetEncoding::Text; // of the response body
    std::chrono::milliseconds timeout = kDefaultNetTimeout; // the whole request
};

// One result for the owner's bus. name: "net.open" {conn}, "net.message" {conn, data}, "net.closed" {conn, reason},
// "net.error" {conn, message}, "net.http.response" {id, status, headers, body}, "net.http.error" {id, message}.
struct NetEvent {
    std::string name;
    nlohmann::json payload;
};

struct NetConnectionStatus {
    std::string name;
    NetType type;
    std::string address; // host:port or the url
    bool tls;
    NetState state;
    uint64_t bytesIn = 0, bytesOut = 0; // on the wire, before TLS
    std::string lastError;
};
struct NetRequestStatus {
    uint64_t id;
    std::string method, url;
};
struct NetStatus {
    std::vector<NetConnectionStatus> connections; // by name
    std::vector<NetRequestStatus> requests;       // in flight, by id
};

// Parsed scheme://host[:port][/target]. nullopt + error if the scheme isn't one of schemes or the host is missing.
struct NetUrl {
    std::string scheme, host;
    uint16_t port;
    std::string target; // "/" if absent
};
std::optional<NetUrl> parseNetUrl(std::string_view url, std::initializer_list<std::string_view> schemes,
                                  std::string& error);

std::string base64Encode(std::string_view bytes);
std::optional<std::string> base64Decode(std::string_view text); // nullopt unless padded standard base64
bool isUtf8(std::string_view text);

class Network {
public:
    Network();
    ~Network(); // stop()
    Network(const Network&) = delete;
    Network& operator=(const Network&) = delete;

    // Starts connecting. Throws std::invalid_argument for an empty name, a name that is connecting or open, or bad
    // options (missing host or port, bad url, unparsable caPem). A closed connection of that name is replaced.
    void open(NetOpenOptions options);
    // Queues bytes (framed as the connection's framing says: u16le prepends the length, line appends '\n'). Throws
    // std::invalid_argument for an unknown name, a closed connection, more than kMaxNetMessage bytes, more than 65535
    // under u16le, or a '\n' under line. Bytes sent while connecting go out once it opens.
    void send(const std::string& name, std::string bytes);
    // Closes it (net.closed "closed by script" follows) and forgets the name. Unknown names are ignored.
    void close(const std::string& name);
    // Starts a request; returns its id (from 1, < 2^53). Throws std::invalid_argument for a bad url or caPem.
    uint64_t request(NetRequestOptions options);

    bool has(const std::string& name) const;
    std::optional<NetState> state(const std::string& name) const;
    std::optional<NetEncoding> encoding(const std::string& name) const;

    // The results since the last poll, oldest first.
    std::vector<NetEvent> poll();
    // Closes every connection (no events), cancels every request, drops queued results and joins the thread. The
    // next open() or request() starts a new one.
    void stop();

    NetStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kast
