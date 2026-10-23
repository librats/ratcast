#pragma once

/**
 * @file fake_portal.h
 * @brief A ScreenCast and Inhibit portal on a test bus, answering as a desktop
 * would.
 *
 * It implements the protocol the way xdg-desktop-portal does — a Request
 * object per call, answered with a Response signal at the path made from the
 * caller's name and token — and records what it was asked, so tests can check
 * the client's side of every exchange without a desktop or a user.
 */

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ratcast::test {

class FakePortal {
public:
    struct Config {
        uint32_t version = 5;
        uint32_t source_types = 1 | 2;      ///< monitors and windows
        uint32_t cursor_modes = 1 | 2 | 4;  ///< hidden, embedded, metadata
        bool answer_start = true;           ///< false: the user never decides
        uint32_t start_response = 0;        ///< 0 granted, 1 cancelled, 2 failed
        uint32_t node = 42;
        std::string restore_token = "restore-1";
        bool inhibits = true;  ///< false: no Inhibit interface
    };

    /// What the portal was asked, as of now.
    struct Seen {
        std::optional<uint32_t> types;
        std::optional<bool> multiple;
        std::optional<uint32_t> cursor_mode;
        std::optional<uint32_t> persist_mode;
        std::optional<std::string> restore_token;
        bool started = false;
        int requests_closed = 0;
        int sessions_closed = 0;
        /// Inhibit: how often, the last one's flags and reason.
        int inhibitions = 0;
        std::optional<uint32_t> inhibit_flags;
        std::string inhibit_reason;
    };

    /// Serve on the bus at `address`; returns once the portal's name is ours.
    FakePortal(const std::string& address, Config config);
    ~FakePortal();

    Seen seen();
    /// Until Start has been called, or a few seconds have passed.
    bool wait_until_started();
    /// Until `done` holds of what was seen, or a few seconds have passed.
    bool wait_until(bool (*done)(const Seen&));
    /// The far end of the PipeWire connection the portal handed out.
    int peer_fd() const noexcept { return peer_fd_; }

    class Service;

private:
    friend class Service;

    const Config config_;
    std::mutex mutex_;
    std::condition_variable changed_;
    Seen seen_;
    bool ready_ = false;
    int peer_fd_ = -1;

    std::unique_ptr<Service> service_;
    std::thread thread_;
};

}  // namespace ratcast::test
