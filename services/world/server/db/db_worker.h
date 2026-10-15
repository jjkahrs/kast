// The only thread that talks to MySQL. The game thread enqueues work; results come back through `post`
// (asio::post onto the io_context), so the tick never blocks on the database.
//
// Saves are not ordinary jobs: they go into a pending map keyed by character (latest position wins) and are
// retried with exponential backoff until they land. A pending save is never dropped, and a character is never
// loaded while its save is still pending.
#pragma once
#include "../core/clock.h"
#include "char_repo.h"
#include "mysql.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace kast::db {

struct WorkerOptions {
    ConnectOptions connect;
    uint32_t realmId = 0;
    Millis retryMin{500}, retryMax{30000};
};

struct SaveStats {
    uint64_t ok = 0;
    uint64_t failedAttempts = 0;
    size_t pending = 0;
};

class DbWorker {
public:
    using Post = std::function<void(std::function<void()>)>;

    DbWorker(WorkerOptions opts, Post post);
    ~DbWorker();

    // Runs `work` on the worker; `done` runs via `post` with nullopt when the DB is unavailable.
    template <typename T>
    void query(std::function<T(CharRepo&)> work, std::function<void(std::optional<T>)> done) {
        submit<T>(std::nullopt, std::move(work), std::move(done));
    }
    // Same, but first lands any pending save for charId (EnterWorld must read the latest position).
    template <typename T>
    void query_for_char(uint64_t charId, std::function<T(CharRepo&)> work, std::function<void(std::optional<T>)> done) {
        submit<T>(charId, std::move(work), std::move(done));
    }

    // Queues (or replaces) the save for s.charId.
    void save(const CharSave& s);

    // Stops taking jobs, flushes pending saves until `deadline`, joins. Returns the saves that could not be flushed.
    size_t stop(TimePoint deadline);

    SaveStats stats() const;

private:
    struct Pending {
        CharSave save;
        uint32_t attempts = 0;
        TimePoint due{};
    };

    template <typename T>
    void submit(std::optional<uint64_t> charId, std::function<T(CharRepo&)> work,
                std::function<void(std::optional<T>)> done) {
        enqueue([this, charId, work = std::move(work), done = std::move(done)] {
            auto result = std::make_shared<std::optional<T>>();
            run_job([&](CharRepo& repo) { *result = work(repo); }, charId);
            post_([done, result] { done(std::move(*result)); });
        });
    }

    void run();
    void enqueue(std::function<void()> job);
    Connection* connection();
    void drop_connection();
    bool attempt_save(uint64_t charId);
    void flush_due_saves(TimePoint now, bool ignoreBackoff);
    // Worker thread: runs work against the DB; false (DB unavailable) on any error.
    bool run_job(const std::function<void(CharRepo&)>& work, std::optional<uint64_t> charId);

    WorkerOptions opts_;
    Post post_;
    std::unique_ptr<Connection> conn_;
    std::optional<TimePoint> lastConnectFailure_;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::map<uint64_t, Pending> pending_;
    bool stopping_ = false;
    TimePoint deadline_{};
    uint64_t savesOk_ = 0, saveFailures_ = 0;
    std::thread thread_;
};

} // namespace kast::db
