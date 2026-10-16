#include "db_worker.h"
#include "../core/log.h"

#include <algorithm>

namespace kast::db {

DbWorker::DbWorker(WorkerOptions opts, Post post) : opts_(std::move(opts)), post_(std::move(post)) {
    thread_ = std::thread([this] { run(); });
}

DbWorker::~DbWorker() {
    if (thread_.joinable()) stop(SteadyClock::now());
}

void DbWorker::enqueue(std::function<void()> job) {
    {
        std::lock_guard lk(mu_);
        if (stopping_) return; // shutting down: no new requests
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void DbWorker::save(const CharSave& s) {
    {
        std::lock_guard lk(mu_);
        auto& p = pending_[s.charId];
        p.save = s; // latest save wins; keeps the retry schedule of an older pending save
        if (p.attempts == 0) p.due = SteadyClock::now();
    }
    cv_.notify_one();
}

size_t DbWorker::stop(TimePoint deadline) {
    {
        std::lock_guard lk(mu_);
        stopping_ = true;
        deadline_ = deadline;
    }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lk(mu_);
    return pending_.size();
}

SaveStats DbWorker::stats() const {
    std::lock_guard lk(mu_);
    return {savesOk_, saveFailures_, pending_.size()};
}

Connection* DbWorker::connection() {
    if (conn_) return conn_.get();
    const auto now = SteadyClock::now();
    // DB down: fail requests fast instead of waiting out a connect timeout for each one.
    if (lastConnectFailure_ && now - *lastConnectFailure_ < Millis(1000)) return nullptr;
    try {
        conn_ = std::make_unique<Connection>(opts_.connect);
        if (lastConnectFailure_) log::info("database reconnected");
        lastConnectFailure_.reset();
    } catch (const DbError& e) {
        if (!lastConnectFailure_) log::warn("database unavailable", {{"error", e.what()}, {"code", e.code}});
        lastConnectFailure_ = now;
    }
    return conn_.get();
}

void DbWorker::drop_connection() { conn_.reset(); }

bool DbWorker::attempt_save(uint64_t charId) {
    CharSave s;
    {
        std::lock_guard lk(mu_);
        auto it = pending_.find(charId);
        if (it == pending_.end()) return true;
        s = it->second.save;
    }
    bool ok = false;
    std::string error = "database unavailable";
    if (Connection* c = connection()) {
        try {
            CharRepo(*c, opts_.realmId).save(s);
            ok = true;
        } catch (const DbError& e) {
            error = e.what();
            drop_connection();
        }
    }
    std::lock_guard lk(mu_);
    auto it = pending_.find(charId);
    if (ok) {
        ++savesOk_;
        // A newer save may have been queued while this one was written; keep it.
        const auto& cur = it->second.save;
        if (cur.pos == s.pos && cur.heading == s.heading && cur.components == s.components) pending_.erase(it);
        else {
            it->second.attempts = 0;
            it->second.due = SteadyClock::now();
        }
    } else {
        ++saveFailures_;
        Pending& p = it->second;
        p.attempts = std::min<uint32_t>(p.attempts + 1, 30);
        const auto backoff = std::min<int64_t>(opts_.retryMax.count(), opts_.retryMin.count() << std::min<uint32_t>(p.attempts - 1, 20));
        p.due = SteadyClock::now() + Millis(backoff);
        log::warn("save failed, will retry", {{"charId", charId}, {"attempt", p.attempts}, {"retryInMs", backoff},
                                              {"error", error}});
    }
    return ok;
}

void DbWorker::flush_due_saves(TimePoint now, bool ignoreBackoff) {
    std::vector<uint64_t> due;
    {
        std::lock_guard lk(mu_);
        for (const auto& [id, p] : pending_)
            if (ignoreBackoff || p.due <= now) due.push_back(id);
    }
    for (uint64_t id : due)
        if (!attempt_save(id)) break; // DB is down; the rest wait for their own retry time
}

bool DbWorker::run_job(const std::function<void(CharRepo&)>& work, std::optional<uint64_t> charId) {
    Connection* c = connection();
    if (!c) return false;
    if (charId && !attempt_save(*charId)) return false;
    c = connection();
    if (!c) return false;
    try {
        CharRepo repo(*c, opts_.realmId);
        work(repo);
        return true;
    } catch (const DbError& e) {
        log::warn("database request failed", {{"error", e.what()}, {"code", e.code}});
        drop_connection();
    } catch (const std::exception& e) {
        log::error("database request failed", {{"error", e.what()}});
    }
    return false;
}

void DbWorker::run() {
    thread_init();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lk(mu_);
            for (;;) {
                if (!jobs_.empty() || stopping_) break;
                if (pending_.empty()) { cv_.wait(lk); continue; }
                TimePoint next = TimePoint::max();
                for (const auto& [id, p] : pending_) next = std::min(next, p.due);
                if (next <= SteadyClock::now()) break;
                cv_.wait_until(lk, next);
            }
            if (stopping_) break;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
        }
        flush_due_saves(SteadyClock::now(), false); // saves first: a logout save lands before the next load
        if (job) job();
    }

    // Shutdown: queued requests are dropped (their sessions are closing); every pending save is retried until the
    // deadline, ignoring backoff.
    for (;;) {
        TimePoint deadline;
        {
            std::lock_guard lk(mu_);
            if (pending_.empty()) break;
            deadline = deadline_;
        }
        if (SteadyClock::now() >= deadline) break;
        lastConnectFailure_.reset();
        flush_due_saves(SteadyClock::now(), true);
        std::unique_lock lk(mu_);
        if (!pending_.empty()) cv_.wait_until(lk, std::min(deadline, SteadyClock::now() + Millis(200)));
    }
    conn_.reset();
    thread_end();
}

} // namespace kast::db
