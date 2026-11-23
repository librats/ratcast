#pragma once

/**
 * @file file_exchange.h
 * @brief A call's files of what is said (chat_files.h): those it has, sent to
 * a member that asks, and those it fetches, asked of members in turn.
 *
 * All of it runs on a thread of its own: what FileTransfer and the session
 * tell it — on their threads, some under way inside a call this thread made
 * (FileTransfer::cancel() finishes the transfer at once) — is queued for that
 * thread, which is also the one that reads a file through to check it. So
 * no lock is held while either is called, nor a network thread kept busy.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "ratcast/core/result.h"
#include "ratcast/session/chat.h"
#include "ratcast/session/media_session.h"

#include "librats/subsystems/file_transfer.h"

namespace ratcast {

class FileExchange {
public:
    struct Handlers {
        /// So much of the file of `id` is here, of `size`; a few times a
        /// second at most.
        std::function<void(const FileId& id, uint64_t received, uint64_t size)> progress;
        /// The file of `id` is here, at the path (UTF-8): fetched, or had
        /// already. Or not: Unavailable — no member had it to send —, or
        /// Cancelled — stop() —.
        std::function<void(const FileId& id, Result<std::string> path)> fetched;
    };

    /// How long a member asked has to offer the file before the next is.
    static constexpr std::chrono::seconds kOfferWithin{15};

    /// Over `transfer` (a node's, which outlives this), asking through
    /// `session`, fetching into `folder` — each file in a folder named by
    /// its id — and asking those `members` names. Sets the transfer's
    /// handlers, which call this one: it goes once the node has stopped.
    FileExchange(librats::FileTransfer& transfer, MediaSession& session, std::string folder,
                 std::function<std::vector<PeerId>()> members, Handlers handlers);
    /// Stops: what is fetched stops coming, nothing more is told.
    ~FileExchange();

    FileExchange(const FileExchange&) = delete;
    FileExchange& operator=(const FileExchange&) = delete;

    /// The file of `id` is at `path` (UTF-8): sent to who asks for it.
    void keep(const FileId& id, std::string path);
    /// Where this side has the file of `id`, if it does.
    std::optional<std::string> path(const FileId& id) const;

    /// Fetch the file `file` describes, asking `first` first, then every
    /// other member. Told by the handlers; at once if it is here already.
    void fetch(const ChatAttachment& file, const PeerId& first);
    /// Fetch it no more: told as Cancelled.
    void stop(const FileId& id);

    // What the session tells: from the network's threads.
    void asked(const PeerId& peer, const FileId& id);
    void none(const PeerId& peer, const FileId& id);
    /// A member gone: what was asked of it is asked of the next.
    void left(const PeerId& peer);

private:
    using SteadyTime = std::chrono::steady_clock::time_point;

    struct Fetch {
        ChatAttachment file;
        std::deque<PeerId> next;           ///< to ask in turn
        std::optional<PeerId> asking;      ///< asked, its offer or answer awaited
        SteadyTime offer_by{};             ///< the next asked if none came by then
        std::optional<uint64_t> transfer;  ///< its offer taken: the transfer's id
        std::string path;                  ///< where it is written then (UTF-8)
        SteadyTime told_at{};              ///< progress last told
        bool stopping = false;
    };

    /// Run `job` on the thread.
    void post(std::function<void()> job);
    void run(std::stop_token stop);

    // On the thread.
    void on_asked(const PeerId& peer, const FileId& id);
    void on_offer(const librats::FileTransfer::Offer& offer);
    void on_progress(const librats::FileTransfer::Progress& progress);
    void on_complete(const PeerId& peer, uint64_t transfer, bool success, const std::string& path);
    void start(const ChatAttachment& file, const PeerId& first);
    /// Ask the next of `fetch`'s members; none left: unavailable.
    void ask_next(const FileId& id);
    void end(const FileId& id, Result<std::string> outcome);
    void time_out();
    /// A fetch whose offer from `peer`, of `size`, is awaited.
    Fetch* awaiting(const PeerId& peer, std::optional<uint64_t> size);
    /// The held file of `id`, if it is still as it was kept.
    std::optional<std::string> held(const FileId& id) const;

    librats::FileTransfer& transfer_;
    MediaSession& session_;
    const std::string folder_;
    const std::function<std::vector<PeerId>()> members_;
    const Handlers handlers_;

    mutable std::mutex held_mutex_;
    std::map<FileId, std::string> held_;

    std::map<FileId, Fetch> fetches_;  ///< the thread's alone
    std::atomic<bool> stopping_{false};

    std::mutex jobs_mutex_;
    std::condition_variable_any jobs_wake_;
    std::deque<std::function<void()>> jobs_;
    std::jthread thread_;  ///< last: started once the rest is
};

}  // namespace ratcast
