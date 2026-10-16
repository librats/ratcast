#pragma once

/**
 * @file loopback.h
 * @brief MediaSessions in one process, joined by a thread instead of a network.
 *
 * What MediaService does over librats, minus librats: the sessions' messages
 * go through a queue that one thread delivers in order, and the same thread
 * ticks every session. Real threads and real time, as the client sees them in
 * the app, without sockets or their timing.
 */

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "ratcast/session/media_session.h"

namespace ratcast::test {

class Loopback {
public:
    Loopback() : thread_([this] { run(); }) {}

    /// Stops delivering, then destroys the sessions: whatever uses them must
    /// be gone first.
    ~Loopback() {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        wake_.notify_one();
        thread_.join();
        parties_.clear();
    }

    /// A session for `id`, owned by the loopback.
    MediaSession& add(const PeerId& id, SessionHandlers handlers) {
        auto party = std::make_unique<Party>(*this, id, std::move(handlers));
        MediaSession& session = party->session;
        std::lock_guard lock(mutex_);
        parties_.emplace(id, std::move(party));
        return session;
    }

    /// Introduce `a` and `b` to each other, as a connection would.
    void connect(const PeerId& a, const PeerId& b) {
        post([this, a, b] {
            session(a).on_peer_connected(b);
            session(b).on_peer_connected(a);
        });
    }

private:
    class Endpoint final : public MediaNetwork {
    public:
        Endpoint(Loopback& loopback, PeerId self) : loopback_(loopback), self_(std::move(self)) {}

        void send_reliable(const PeerId& peer, const Buffer& message) override {
            send(peer, message);
        }
        bool send_live(const PeerId& peer, const Buffer& message, Duration) override {
            send(peer, message);
            return true;
        }
        bool writable(const PeerId&) const override { return true; }
        size_t max_live_message() const override { return 4 * 1024 * 1024; }

    private:
        void send(const PeerId& peer, const Buffer& message) {
            loopback_.post([this, peer, message] {
                loopback_.session(peer).on_message(self_, message.bytes());
            });
        }

        Loopback& loopback_;
        const PeerId self_;
    };

    struct Party {
        Party(Loopback& loopback, const PeerId& id, SessionHandlers handlers)
            : endpoint(loopback, id),
              session(endpoint, SteadyClock::instance(), std::move(handlers)) {}
        Endpoint endpoint;
        MediaSession session;
    };

    MediaSession& session(const PeerId& id) {
        std::lock_guard lock(mutex_);
        return parties_.at(id)->session;
    }

    void post(std::function<void()> work) {
        {
            std::lock_guard lock(mutex_);
            work_.push_back(std::move(work));
        }
        wake_.notify_one();
    }

    void run() {
        auto next_tick = std::chrono::steady_clock::now();
        std::unique_lock lock(mutex_);
        while (running_) {
            if (!work_.empty()) {
                std::function<void()> work = std::move(work_.front());
                work_.pop_front();
                lock.unlock();
                work();
                lock.lock();
                continue;
            }
            if (std::chrono::steady_clock::now() >= next_tick) {
                next_tick += std::chrono::milliseconds(10);
                for (auto& [id, party] : parties_) {
                    lock.unlock();
                    party->session.tick();
                    lock.lock();
                }
                continue;
            }
            wake_.wait_until(lock, next_tick);
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> work_;
    std::map<PeerId, std::unique_ptr<Party>> parties_;
    bool running_ = true;
    std::thread thread_;  // last: it runs on everything above
};

}  // namespace ratcast::test
