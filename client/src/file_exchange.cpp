#include "file_exchange.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

#include "ratcast/client/chat_files.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

namespace fs = std::filesystem;
using Transfer = librats::FileTransfer;

fs::path path_of(const std::string& utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/// A file's id as a log shows it.
std::string short_of(const FileId& id) { return to_hex(id).substr(0, 8); }

constexpr auto kProgressEvery = std::chrono::milliseconds(200);

}  // namespace

FileExchange::FileExchange(librats::FileTransfer& transfer, MediaSession& session,
                           std::string folder, std::function<std::vector<PeerId>()> members,
                           Handlers handlers)
    : transfer_(transfer),
      session_(session),
      folder_(std::move(folder)),
      members_(std::move(members)),
      handlers_(std::move(handlers)) {
    transfer_.on_offer(
        [this](const Transfer::Offer& offer) { post([this, offer] { on_offer(offer); }); });
    // A transfer received ends with its progress, Completed or not: the
    // completion handler, which outgoing transfers share, cannot tell an
    // incoming one from an outgoing one of the same peer and number.
    transfer_.on_progress([this](const Transfer::Progress& progress) {
        if (progress.direction == Transfer::Direction::Receiving)
            post([this, progress] { on_progress(progress); });
    });
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

FileExchange::~FileExchange() {
    stopping_ = true;
    thread_.request_stop();
    jobs_wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FileExchange::keep(const FileId& id, std::string path) {
    std::lock_guard lock(held_mutex_);
    held_[id] = std::move(path);
}

std::optional<std::string> FileExchange::path(const FileId& id) const { return held(id); }

std::optional<std::string> FileExchange::held(const FileId& id) const {
    std::string path;
    {
        std::lock_guard lock(held_mutex_);
        const auto it = held_.find(id);
        if (it == held_.end()) return std::nullopt;
        path = it->second;
    }
    std::error_code error;
    if (!fs::is_regular_file(path_of(path), error)) return std::nullopt;
    return path;
}

void FileExchange::fetch(const ChatAttachment& file, const PeerId& first) {
    post([this, file, first] { start(file, first); });
}

void FileExchange::stop(const FileId& id) {
    post([this, id] {
        const auto it = fetches_.find(id);
        if (it == fetches_.end()) return;
        Fetch& fetch = it->second;
        if (fetch.transfer && fetch.asking) {
            // Ends as its progress says, Cancelled.
            fetch.stopping = true;
            transfer_.cancel(*fetch.asking, *fetch.transfer);
            return;
        }
        end(id, fail(Errc::Cancelled));
    });
}

void FileExchange::asked(const PeerId& peer, const FileId& id) {
    post([this, peer, id] { on_asked(peer, id); });
}

void FileExchange::none(const PeerId& peer, const FileId& id) {
    post([this, peer, id] {
        const auto it = fetches_.find(id);
        if (it == fetches_.end() || it->second.asking != peer || it->second.transfer) return;
        LOG_DEBUG("files", peer.short_hex() << " has no " << short_of(id));
        ask_next(id);
    });
}

void FileExchange::left(const PeerId& peer) {
    post([this, peer] {
        std::vector<FileId> asked;
        for (const auto& [id, fetch] : fetches_) {
            // One under way ends as its transfer fails.
            if (fetch.asking == peer && !fetch.transfer) asked.push_back(id);
        }
        for (const FileId& id : asked) ask_next(id);
    });
}

// ── The thread ──────────────────────────────────────────────────────────────

void FileExchange::post(std::function<void()> job) {
    {
        std::lock_guard lock(jobs_mutex_);
        jobs_.push_back(std::move(job));
    }
    jobs_wake_.notify_one();
}

void FileExchange::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::deque<std::function<void()>> jobs;
        {
            std::unique_lock lock(jobs_mutex_);
            // Awake for the next offer to come, or for a second at most.
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            for (const auto& [id, fetch] : fetches_) {
                if (fetch.asking && !fetch.transfer) until = std::min(until, fetch.offer_by);
            }
            jobs_wake_.wait_until(lock, stop, until, [this] { return !jobs_.empty(); });
            jobs.swap(jobs_);
        }
        for (std::function<void()>& job : jobs) {
            if (stop.stop_requested()) return;
            job();
        }
        time_out();
    }
}

void FileExchange::on_asked(const PeerId& peer, const FileId& id) {
    if (const std::optional<std::string> path = held(id)) {
        if (transfer_.send_file(peer, *path) != 0) {
            LOG_INFO("files", "sending " << short_of(id) << " to " << peer.short_hex());
            return;
        }
    }
    session_.no_file(peer, id);
}

void FileExchange::start(const ChatAttachment& file, const PeerId& first) {
    if (fetches_.contains(file.id)) return;
    if (std::optional<std::string> path = held(file.id)) {
        if (handlers_.fetched) handlers_.fetched(file.id, std::move(*path));
        return;
    }
    Fetch& fetch = fetches_[file.id];
    fetch.file = file;
    fetch.next.push_back(first);
    for (const PeerId& member : members_()) {
        if (member != first) fetch.next.push_back(member);
    }
    LOG_INFO("files", "fetching " << short_of(file.id) << ", " << file.size << " bytes, of "
                                  << fetch.next.size() << " member(s)");
    ask_next(file.id);
}

void FileExchange::ask_next(const FileId& id) {
    const auto it = fetches_.find(id);
    if (it == fetches_.end()) return;
    Fetch& fetch = it->second;
    fetch.asking.reset();
    fetch.transfer.reset();
    if (fetch.stopping) {
        end(id, fail(Errc::Cancelled));
        return;
    }
    while (!fetch.next.empty()) {
        const PeerId peer = fetch.next.front();
        fetch.next.pop_front();
        // An offer is told from another of the same size by whom it is
        // awaited of: one such of a member at a time.
        if (awaiting(peer, fetch.file.size)) {
            fetch.next.push_back(peer);
            if (std::ranges::all_of(fetch.next, [&](const PeerId& p) {
                    return awaiting(p, fetch.file.size) != nullptr;
                })) {
                // All busy: tried again as one of theirs comes.
                fetch.offer_by = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                return;
            }
            continue;
        }
        if (session_.ask_file(peer, id)) {
            fetch.asking = peer;
            fetch.offer_by = std::chrono::steady_clock::now() + kOfferWithin;
            return;
        }
    }
    LOG_INFO("files", "nobody here has " << short_of(id));
    end(id, fail(Errc::Unavailable));
}

FileExchange::Fetch* FileExchange::awaiting(const PeerId& peer, std::optional<uint64_t> size) {
    for (auto& [id, fetch] : fetches_) {
        if (fetch.asking == peer && !fetch.transfer && (!size || fetch.file.size == *size))
            return &fetch;
    }
    return nullptr;
}

void FileExchange::on_offer(const Transfer::Offer& offer) {
    Fetch* fetch = offer.is_directory ? nullptr : awaiting(offer.from, offer.size);
    if (!fetch) {
        transfer_.reject(offer.from, offer.id);
        return;
    }
    // Under its name as this system can have it, in a folder of its id: no
    // other file of the room is in the way, nor anything outside written.
    const fs::path folder = path_of(folder_) / to_hex(fetch->file.id);
    std::error_code error;
    fs::create_directories(folder, error);
    fetch->transfer = offer.id;
    fetch->path = utf8_of(folder / path_of(local_file_name(fetch->file.name)));
    transfer_.accept(offer.from, offer.id, fetch->path);
}

void FileExchange::on_progress(const Transfer::Progress& progress) {
    const auto it = std::ranges::find_if(fetches_, [&](const auto& entry) {
        return entry.second.asking == progress.peer && entry.second.transfer == progress.id;
    });
    if (it == fetches_.end()) return;
    const FileId id = it->first;
    Fetch& fetch = it->second;
    switch (progress.status) {
        case Transfer::Status::Pending:
        case Transfer::Status::Active:
        case Transfer::Status::Paused: {
            const auto now = std::chrono::steady_clock::now();
            if (now - fetch.told_at < kProgressEvery) return;
            fetch.told_at = now;
            if (handlers_.progress)
                handlers_.progress(id, progress.bytes_transferred, progress.total_bytes);
            return;
        }
        case Transfer::Status::Completed: {
            const std::string path = fetch.path;
            // Whatever it sent, it is kept only as what was asked for.
            const Result<FileId> came = file_id(path, &stopping_);
            if (came && *came == id) {
                keep(id, path);
                end(id, path);
                return;
            }
            std::error_code error;
            fs::remove(path_of(path), error);
            if (!came && came.error() == Errc::Cancelled) return;
            LOG_WARN("files", progress.peer.short_hex()
                                  << " sent another file for " << short_of(id));
            ask_next(id);
            return;
        }
        case Transfer::Status::Failed:
        case Transfer::Status::Cancelled:
            LOG_INFO("files", short_of(id)
                                  << " from " << progress.peer.short_hex()
                                  << (progress.status == Transfer::Status::Failed ? " failed"
                                                                                  : " stopped"));
            ask_next(id);
            return;
    }
}

void FileExchange::end(const FileId& id, Result<std::string> outcome) {
    fetches_.erase(id);
    if (handlers_.fetched) handlers_.fetched(id, std::move(outcome));
}

void FileExchange::time_out() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<FileId> late;
    for (const auto& [id, fetch] : fetches_) {
        // No offer in time — or, every member left busy with another of the
        // same size, a second waited: the next asked.
        if (!fetch.transfer && fetch.offer_by != SteadyTime{} && now >= fetch.offer_by)
            late.push_back(id);
    }
    for (const FileId& id : late) {
        const auto it = fetches_.find(id);
        if (it == fetches_.end()) continue;
        if (const std::optional<PeerId>& asked = it->second.asking)
            LOG_INFO("files", asked->short_hex() << " offered no " << short_of(id));
        ask_next(id);
    }
}

}  // namespace ratcast
