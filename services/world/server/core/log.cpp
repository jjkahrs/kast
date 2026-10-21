// kast::log on Quill: the caller builds the JSON line and enqueues it; Quill's backend thread writes it to stdout.
#include "log.h"
#include "utc.h"

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/Sink.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <mutex>

namespace kast::log {
namespace {
std::atomic<Level> g_level{Level::info};
constexpr const char* kNames[] = {"debug", "info", "warn", "error"};

// Per caller thread queue: grows to 64 MiB, then the caller blocks until the backend catches up; never drops.
struct QueueOptions {
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
    static constexpr size_t initial_queue_capacity = 128u * 1024u;
    static constexpr uint32_t blocking_queue_retry_interval_ns = 800;
    static constexpr size_t unbounded_queue_max_capacity = 64u * 1024u * 1024u;
    static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Never;
};
using Frontend = quill::FrontendImpl<QueueOptions>;
using Logger = quill::LoggerImpl<QueueOptions>;

// One metadata per level; the message is the finished JSON line.
constexpr quill::MacroMetadata kLine[] = {
    {"log.cpp:debug", "", "{}", nullptr, quill::LogLevel::Debug, quill::MacroMetadata::Event::Log},
    {"log.cpp:info", "", "{}", nullptr, quill::LogLevel::Info, quill::MacroMetadata::Event::Log},
    {"log.cpp:warn", "", "{}", nullptr, quill::LogLevel::Warning, quill::MacroMetadata::Event::Log},
    {"log.cpp:error", "", "{}", nullptr, quill::LogLevel::Error, quill::MacroMetadata::Event::Log}};

std::string dump(const nlohmann::json& j) {
    // replace: invalid UTF-8 from a client never throws inside logging
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// {"ts":..,"level":..,"msg":.., then the other fields}; fields' ts/level/msg replace the given ones in place.
std::string json_line(nlohmann::json ts, nlohmann::json level, nlohmann::json msg, nlohmann::json fields) {
    const std::pair<const char*, nlohmann::json*> reserved[] = {{"ts", &ts}, {"level", &level}, {"msg", &msg}};
    if (fields.is_object())
        for (auto [key, slot] : reserved)
            if (auto it = fields.find(key); it != fields.end()) {
                *slot = std::move(*it);
                fields.erase(it);
            }
    std::string line = R"({"ts":)" + dump(ts) + R"(,"level":)" + dump(level) + R"(,"msg":)" + dump(msg);
    if (fields.is_object() && !fields.empty()) line += ',' + dump(fields).substr(1);
    else line += '}';
    return line;
}

const char* name_of(quill::LogLevel l) {
    if (l <= quill::LogLevel::Debug) return "debug";
    if (l <= quill::LogLevel::Notice) return "info";
    return l == quill::LogLevel::Warning ? "warn" : "error";
}

// Runs on the backend thread: stdout (stdio-buffered, flushed by the backend when idle) and the test sink.
class JsonStdoutSink final : public quill::Sink {
public:
    void set_test_sink(std::function<void(const std::string&)> sink) {
        std::lock_guard lock(m_);
        test_ = std::move(sink);
    }

protected:
    void write_log(const quill::MacroMetadata* meta, uint64_t ts, std::string_view, std::string_view,
                   const std::string&, std::string_view, quill::LogLevel level, std::string_view, std::string_view,
                   const std::vector<std::pair<std::string, std::string>>*, std::string_view message,
                   std::string_view) override {
        std::string line;
        if (meta >= std::begin(kLine) && meta < std::end(kLine)) {
            line = message;
        } else { // Quill's own lines (the crash handler): wrap them in the same JSON shape
            const auto t = std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(ts)));
            line = json_line(utc_iso8601(t), name_of(level), message, nullptr);
        }
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), stdout);
        line.pop_back();
        std::lock_guard lock(m_);
        if (test_) test_(line);
    }
    void flush_sink() override { std::fflush(stdout); }

private:
    std::mutex m_;
    std::function<void(const std::string&)> test_;
};

struct Backend {
    std::shared_ptr<JsonStdoutSink> sink;
    Logger* logger = nullptr;
};

// Started on the first line (or flush/set_test_sink), so startup and config errors log too. Quill's atexit handler
// writes everything still queued when the process exits normally.
Backend& backend() {
    static Backend b = [] {
        quill::BackendOptions options;
        options.check_printable_char = {}; // lines are already JSON-escaped UTF-8
        options.thread_name = "kast_log";
        options.error_notifier = [](const std::string& e) { // ponytail: queue growth notices are dropped
            if (e.find("Quill INFO") == std::string::npos) std::fprintf(stderr, "%s\n", e.c_str());
        };
        quill::SignalHandlerOptions signals;
        signals.logger_name = "kast";
#ifdef _WIN32
        signals.catchable_signals = {SIGABRT, SIGFPE, SIGILL, SIGSEGV};
#else
        // SIGINT/SIGTERM: only until main installs its graceful-shutdown handler (asio::signal_set replaces these).
        signals.catchable_signals = {SIGINT, SIGTERM, SIGABRT, SIGFPE, SIGILL, SIGSEGV};
#endif
        quill::Backend::start<QueueOptions>(options, signals);
#ifdef _WIN32
        // Windows start() hooks crashes (SEH) and Ctrl+C. Ctrl+C would std::exit past the graceful shutdown, so it
        // goes; abort() and the CRT crash signals are hooked like on Linux.
        SetConsoleCtrlHandler(quill::detail::on_console_signal<QueueOptions>, FALSE);
        quill::init_signal_handler<QueueOptions>(signals.catchable_signals);
#endif
        Backend made;
        made.sink = std::static_pointer_cast<JsonStdoutSink>(Frontend::create_or_get_sink<JsonStdoutSink>("kast_stdout"));
        made.logger = Frontend::create_or_get_logger(
            "kast", made.sink, quill::PatternFormatterOptions{"%(message)"});
        made.logger->set_log_level(quill::LogLevel::Debug); // g_level filters before anything is queued
        return made;
    }();
    return b;
}
} // namespace

void set_level(Level l) { g_level = l; }

void set_test_sink(std::function<void(const std::string& line)> sink) { backend().sink->set_test_sink(std::move(sink)); }

void flush() { backend().logger->flush_log(); }

bool parse_level(std::string_view s, Level& out) {
    for (int i = 0; i < 4; ++i)
        if (s == kNames[i]) { out = static_cast<Level>(i); return true; }
    return false;
}

void write(Level l, std::string_view msg, nlohmann::json fields) {
    if (l < g_level.load()) return;
    // C++ callers may still override ts/level/msg through fields.
    const std::string line =
        json_line(utc_iso8601(std::chrono::system_clock::now()), kNames[static_cast<int>(l)], msg, std::move(fields));
    backend().logger->log_statement<false>(&kLine[static_cast<int>(l)], std::string_view(line));
}

} // namespace kast::log
