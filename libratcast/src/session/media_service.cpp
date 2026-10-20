#include "ratcast/session/media_service.h"

#include <algorithm>
#include <span>
#include <utility>

#include "librats/core/types.h"
#include "librats/peer/peer.h"
#include "librats/wire/frame.h"

namespace ratcast {
namespace {

librats::ByteView view_of(const Buffer& buffer) noexcept {
    return librats::ByteView(buffer.data(), buffer.size());
}

}  // namespace

MediaService::MediaService(SessionHandlers handlers, SessionConfig config, Duration tick_interval)
    : tick_interval_(tick_interval),
      // The MediaNetwork base is constructed before any member, so the session
      // may hold on to it already.
      session_(static_cast<MediaNetwork&>(*this), SteadyClock::instance(), std::move(handlers),
               config) {}

MediaService::~MediaService() { stop(); }

void MediaService::attach(librats::NodeContext& ctx) {
    librats::PeerNetwork& network = ctx.network;
    network.on(librats::MessageType::Media,
               [this](const librats::Peer& peer, librats::ByteView bytes) {
                   session_.on_message(peer.id(), std::span(bytes.data(), bytes.size()));
               });
    network.on_peer_connected(
        [this](const librats::Peer& peer) { session_.on_peer_connected(peer.id()); });
    network.on_peer_disconnected([this](const librats::PeerId& peer, librats::CloseReason) {
        session_.on_peer_disconnected(peer);
    });
    network.on_peer_writable(
        [this](const librats::Peer& peer) { session_.on_peer_writable(peer.id()); });
    // Turns path sampling on, every NodeConfig::path_sample_ms (100 ms if 0).
    network.on_peer_path([this](const librats::Peer& peer, const librats::PathStats& sample) {
        session_.on_peer_path(peer.id(), sample);
    });
    network.on_live_outcome(librats::MessageType::Media,
                            [this](const librats::Peer& peer, uint64_t tag, bool delivered) {
                                session_.on_live_outcome(peer.id(), tag, delivered);
                            });
    network_.store(&network, std::memory_order_release);
}

void MediaService::start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&MediaService::run, this);
}

void MediaService::stop() {
    if (!running_.exchange(false)) return;
    {
        // Under the lock, so the notification cannot fall between run()'s check
        // of running_ and its wait.
        std::lock_guard lock(wait_mutex_);
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void MediaService::run() {
    std::unique_lock lock(wait_mutex_);
    while (running_.load()) {
        wake_.wait_for(lock, tick_interval_, [this] { return !running_.load(); });
        if (!running_.load()) break;
        lock.unlock();
        session_.tick();
        lock.lock();
    }
}

void MediaService::send_reliable(const PeerId& peer, const Buffer& message) {
    if (librats::PeerNetwork* network = network_.load(std::memory_order_acquire)) {
        network->send(peer, librats::MessageType::Media, view_of(message));
    }
}

bool MediaService::send_live(const PeerId& peer, const Buffer& message, Duration lifetime,
                             uint64_t tag) {
    // librats counts lifetimes in milliseconds; round up so a frame never gets
    // less time than its stream allows.
    librats::SendOptions options;
    options.lifetime = std::max(std::chrono::ceil<std::chrono::milliseconds>(lifetime),
                                std::chrono::milliseconds(1));
    options.tag = tag;
    librats::PeerNetwork* network = network_.load(std::memory_order_acquire);
    return network && network->send(peer, librats::MessageType::Media, view_of(message), options);
}

bool MediaService::writable(const PeerId& peer) const {
    const librats::PeerNetwork* network = network_.load(std::memory_order_acquire);
    return network && network->peer_writable(peer);
}

size_t MediaService::max_live_message() const { return librats::SendOptions::kMaxLivePayload; }

}  // namespace ratcast
