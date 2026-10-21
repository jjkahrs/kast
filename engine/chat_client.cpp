#include "chat_client.h"

#include "log.h"

#include <codec.h>

#include <algorithm>
#include <format>
#include <stdexcept>

namespace kast {
namespace {

using Json = nlohmann::json;
namespace proto = kast::proto;

constexpr size_t kMaxPayload = 0xFFFF; // the u16 length already caps a server frame

const char* targetName(proto::ChatTarget t)
{
    switch (t) {
    case proto::ChatTarget::whisper: return "whisper";
    case proto::ChatTarget::channel: return "channel";
    case proto::ChatTarget::zone: return "zone";
    case proto::ChatTarget::whisper_sent: return "whisper_sent";
    }
    return "unknown";
}

// Confirmations become chat.notice; everything else (a request refused or a message dropped) chat.error.
bool isInfo(proto::ChatNoticeCode c)
{
    using enum proto::ChatNoticeCode;
    return c == ignored || c == unignored || c == mute_set || c == mute_cleared || c == you_muted || c == you_unmuted;
}

} // namespace

const char* chatStateName(ChatState state)
{
    switch (state) {
    case ChatState::Disconnected: return "disconnected";
    case ChatState::Connecting: return "connecting";
    case ChatState::Connected: return "connected";
    }
    return "disconnected";
}

ChatClient::ChatClient(Network& network, AuthClient& auth) : network_(network), auth_(auth) {}

void ChatClient::reset(std::optional<AuthConfig> config)
{
    if (!conn_.empty())
        network_.close(conn_);
    enabled_ = config.has_value();
    caPem_ = config ? config->caPem : std::string();
    state_ = ChatState::Disconnected;
    target_.reset();
    conn_.clear();
    inbox_.clear();
    channels_.clear();
    wanted_.clear();
    backoff_ = kRetryMin;
    retryAt_.reset();
    lastStatus_.reset();
    events_.clear();
}

void ChatClient::require(const char* function) const
{
    if (!enabled_)
        throw std::logic_error(std::format("{}: Online Services are disabled", function));
    if (state_ != ChatState::Connected)
        throw std::logic_error(std::format("{}: needs the chat service connected, it is '{}'", function, chatStateName(state_)));
}

void ChatClient::whisper(const std::string& name, const std::string& text)
{
    require("whisper");
    send(proto::ChatSend{proto::ChatTarget::whisper, name, text});
}

void ChatClient::sendChannel(const std::string& channel, const std::string& text)
{
    require("send");
    send(proto::ChatSend{proto::ChatTarget::channel, channel, text});
}

void ChatClient::zone(const std::string& text)
{
    require("zone");
    send(proto::ChatSend{proto::ChatTarget::zone, "", text});
}

void ChatClient::join(const std::string& channel)
{
    require("join");
    send(proto::ChatJoin{channel});
}

void ChatClient::leave(const std::string& channel)
{
    require("leave");
    send(proto::ChatLeave{channel});
}

void ChatClient::ignore(const std::string& name, bool on)
{
    require(on ? "ignore" : "unignore");
    send(proto::ChatIgnore{name, on});
}

void ChatClient::mute(const std::string& name, uint32_t minutes)
{
    require(minutes ? "mute" : "unmute");
    send(proto::ChatMute{name, minutes});
}

std::string ChatClient::channelName(int n, const char* function) const
{
    const auto it = channels_.find(n);
    if (it == channels_.end())
        throw std::logic_error(std::format("{}: not in a channel numbered {}", function, n));
    return it->second;
}

void ChatClient::observe(const NetEvent& worldEvent)
{
    if (!enabled_)
        return;
    if (worldEvent.name == "world.chatServer") {
        auto target = std::pair(worldEvent.payload.value("host", ""), worldEvent.payload.value("port", uint16_t{0}));
        if (state_ != ChatState::Disconnected && target == target_)
            return; // the same service: the connection stays
        target_ = std::move(target);
        if (state_ != ChatState::Disconnected) // somewhere else now: start over
            drop("chat service moved");
        backoff_ = kRetryMin;
        retryAt_ = std::chrono::steady_clock::now(); // at the next poll
    } else if (worldEvent.name == "world.disconnected") {
        target_.reset();
        retryAt_.reset();
        if (state_ != ChatState::Disconnected)
            drop("world disconnected");
        wanted_.clear();
        lastStatus_.reset();
    }
}

void ChatClient::open()
{
    retryAt_.reset();
    const std::optional<std::string> token = auth_.accessToken(); // the current one: refreshed by AuthClient
    if (!target_ || !token)
        return drop(token ? "no chat service" : "logged out");
    conn_ = std::format("{}{}", kConnPrefix, ++serial_);
    NetOpenOptions options;
    options.name = conn_;
    options.type = NetType::Tcp;
    options.host = target_->first;
    options.port = target_->second;
    options.tls = true;
    options.caPem = caPem_;
    options.encoding = NetEncoding::Base64;
    options.framing = NetFraming::Raw;
    try {
        network_.open(std::move(options));
    } catch (const std::invalid_argument& e) {
        return drop(e.what());
    }
    state_ = ChatState::Connecting;
    log::info(std::format("Chat: connecting to {}:{}", target_->first, target_->second));
    send(proto::Hello{});
    send(proto::Auth{*token}); // queued until TLS is up
}

void ChatClient::send(const auto& message)
{
    const std::vector<uint8_t> frame = proto::encode(proto::Message{message});
    network_.send(conn_, std::string(frame.begin(), frame.end()));
}

bool ChatClient::handle(const NetEvent& event)
{
    const std::string conn = event.payload.value("conn", "");
    if (!conn.starts_with(kConnPrefix))
        return false;
    if (conn != conn_)
        return true; // an old connection's late result
    if (event.name == "net.message") {
        if (std::optional<std::string> bytes = base64Decode(event.payload.value("data", "")))
            received(*bytes);
    } else if (event.name == "net.closed") {
        drop(event.payload.value("reason", "closed"));
    }
    return true;
}

void ChatClient::received(const std::string& bytes)
{
    inbox_ += bytes;
    size_t at = 0;
    while (!conn_.empty()) {
        const auto* data = reinterpret_cast<const uint8_t*>(inbox_.data()) + at;
        const proto::Decoded frame = proto::decode_frame({data, inbox_.size() - at}, kMaxPayload);
        if (frame.status == proto::DecodeStatus::need_more)
            break;
        if (frame.status != proto::DecodeStatus::ok)
            return drop("the chat service sent a frame this client can't read");
        at += frame.consumed;
        std::visit([this](const auto& m) { dispatch(m); }, frame.msg);
    }
    if (!conn_.empty())
        inbox_.erase(0, at);
}

void ChatClient::dispatch(const auto& m)
{
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, proto::AuthOk>) {
        state_ = ChatState::Connected;
        backoff_ = kRetryMin;
        log::info("Chat: connected");
        status(true, "");
        for (const std::string& channel : wanted_) // the service put it in General again itself
            send(proto::ChatJoin{channel});
    } else if constexpr (std::is_same_v<T, proto::Disconnect>) {
        drop(proto::code_name(m.reason));
    } else if constexpr (std::is_same_v<T, proto::Ping>) {
        send(proto::Pong{m.nonce});
    } else if constexpr (std::is_same_v<T, proto::ChatLine>) {
        Json e{{"kind", targetName(m.target)}, {"text", m.text}}; // never logged
        e[m.target == proto::ChatTarget::whisper_sent ? "to" : "from"] = m.name;
        if (m.target == proto::ChatTarget::channel) {
            e["channel"] = m.channel;
            for (const auto& [n, name] : channels_)
                if (name == m.channel)
                    e["number"] = n;
        }
        emit("chat.message", std::move(e));
    } else if constexpr (std::is_same_v<T, proto::ChatChannel>) {
        if (m.joined) {
            channels_[m.number] = m.channel;
            if (m.number != 1)
                wanted_.insert(m.channel);
        } else {
            channels_.erase(m.number);
            wanted_.erase(m.channel);
        }
        emit("chat.channel", {{"channel", m.channel}, {"number", m.number}, {"joined", m.joined}});
    } else if constexpr (std::is_same_v<T, proto::ChatNotice>) {
        emit(isInfo(m.code) ? "chat.notice" : "chat.error",
             {{"code", proto::chat_notice_name(m.code)}, {"name", m.name}, {"minutes", m.minutes}});
    } else if constexpr (std::is_same_v<T, proto::Pong>) {
    } else {
        drop(std::format("the chat service sent {}", proto::opcode_name(T::op)));
    }
}

void ChatClient::drop(const std::string& reason)
{
    if (!conn_.empty())
        network_.close(conn_);
    conn_.clear();
    inbox_.clear();
    channels_.clear();
    const bool was = state_ == ChatState::Connected;
    state_ = ChatState::Disconnected;
    if (was || lastStatus_ != false)
        log::info(std::format("Chat: disconnected: {}", reason));
    status(false, reason);
    if (target_) {
        retryAt_ = std::chrono::steady_clock::now() + backoff_;
        backoff_ = std::min<std::chrono::steady_clock::duration>(backoff_ * 2, kRetryMax);
    }
}

void ChatClient::status(bool connected, const std::string& reason)
{
    if (lastStatus_ == connected)
        return;
    lastStatus_ = connected;
    Json e{{"connected", connected}};
    if (!connected)
        e["reason"] = reason;
    emit("chat.status", std::move(e));
}

void ChatClient::emit(std::string name, Json payload) { events_.push_back({std::move(name), std::move(payload)}); }

std::vector<NetEvent> ChatClient::poll()
{
    if (enabled_ && state_ == ChatState::Disconnected && retryAt_ && std::chrono::steady_clock::now() >= *retryAt_)
        open();
    std::vector<NetEvent> out;
    out.swap(events_);
    return out;
}

} // namespace kast
