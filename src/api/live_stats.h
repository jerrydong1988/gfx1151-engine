// In-memory observations of active generations. Independent of reqstat history:
// no disk scans, engine queries, model data or GPU synchronization on a poll.
#pragma once
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include "engine_client.h"
#include "nlohmann/json.hpp"

namespace live_stats {
using json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
inline double now_ms() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}
class Store {
    struct Request {
        long long prompt = 0, generated = 0;
        gdec::GenProgress pre;
        bool reported = false;
        double started = now_ms(), updated = started, decode_started = 0;
    };
    std::mutex mutex_;
    std::map<long long, Request> requests_;
    uint64_t completed_ = 0, estimated_ = 0, generated_ = 0;
    json last_prefill_ = nullptr;
    // A process identity lets browsers reset delta baselines after a restart.
    const double started_ = now_ms();
    const std::string session_ = std::to_string(
        std::chrono::system_clock::now().time_since_epoch().count());
public:
    void start(long long id, long long prompt) {
        std::lock_guard<std::mutex> lock(mutex_);
        Request r;
        r.prompt = prompt;
        requests_.emplace(id, r);
    }
    void progress(long long id, const gdec::GenProgress& p) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = requests_.find(id);
        if (it == requests_.end()) return;
        auto& r = it->second;
        if (r.pre.prefill_done || p.completed < r.pre.completed ||
            p.estimated < r.pre.estimated) return;
        completed_ += p.completed - r.pre.completed;
        estimated_ += p.estimated - r.pre.estimated;
        r.pre = p;
        r.reported = true;
        r.updated = now_ms();
        if (p.prefill_done) {
            r.decode_started = r.updated;
            last_prefill_ = {{"req_seq", id}, {"tokens", p.total},
                {"cached_tokens", p.cached}, {"prefill_ms", p.prefill_ms},
                {"tok_per_s", p.total > 0 && p.prefill_ms > 0
                    ? json(p.total * 1000.0 / p.prefill_ms) : json(nullptr)}};
        }
    }
    void token(long long id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = requests_.find(id);
        if (it == requests_.end()) return;
        auto& r = it->second;
        r.updated = now_ms();
        if (!r.decode_started) r.decode_started = r.updated;
        ++r.generated;
        ++generated_;
    }
    void finish(long long id) {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.erase(id);
    }
    json snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        const double now = now_ms();
        json active = json::array();
        for (const auto& [id, r] : requests_) {
            const auto& p = r.pre;
            active.push_back({{"req_seq", id},
                {"phase", r.decode_started ? "decode" : r.reported ? "prefill" : "starting"},
                {"prompt_tokens", r.prompt}, {"output_tokens", r.generated},
                {"prefill_reported", r.reported}, {"prefill_done", p.prefill_done},
                {"prefill_completed_tokens", r.reported ? json(p.completed) : json(nullptr)},
                {"prefill_estimated_tokens", r.reported ? json(p.estimated) : json(nullptr)},
                {"prefill_total_tokens", r.reported ? json(p.total) : json(nullptr)},
                {"cached_tokens", r.reported ? json(p.cached) : json(nullptr)},
                {"prefill_ms", r.reported ? json(p.prefill_ms) : json(nullptr)},
                {"prefill_tok_per_s", r.reported && p.estimated > 0 && p.prefill_ms > 0
                    ? json(p.estimated * 1000.0 / p.prefill_ms) : json(nullptr)},
                {"elapsed_ms", now - r.started}, {"update_age_ms", now - r.updated}});
        }
        return {{"session", session_}, {"sample_ms", now - started_},
            {"prefill_completed_tokens_total", completed_},
            {"prefill_estimated_tokens_total", estimated_},
            {"output_tokens_total", generated_}, {"active", active},
            {"last_prefill", last_prefill_}};
    }
};
class Lease {
    Store& store_;
    long long id_;
public:
    Lease(Store& store, long long id, long long prompt) : store_(store), id_(id) {
        store_.start(id, prompt);
    }
    ~Lease() { store_.finish(id_); }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
};
}  // namespace live_stats
