#include "ratcast/client/voice_meter.h"

#include <algorithm>

namespace ratcast {

VoiceMeter::VoiceMeter(Clock& clock, Duration break_after)
    : clock_(clock), break_after_(break_after), started_(clock.now()) {}

void VoiceMeter::start() {
    std::lock_guard lock(mutex_);
    started_ = clock_.now();
    meters_.clear();
}

void VoiceMeter::heard(const PeerId& peer, std::optional<Duration> latency) {
    std::lock_guard lock(mutex_);
    std::unique_ptr<PlaybackMeter>& meter = meters_[peer];
    if (!meter) {
        meter = std::make_unique<PlaybackMeter>(clock_, break_after_);
        meter->start(started_);
    }
    meter->picture(latency);
}

std::map<PeerId, PlaybackStats> VoiceMeter::voices() const {
    std::lock_guard lock(mutex_);
    std::map<PeerId, PlaybackStats> voices;
    for (const auto& [peer, meter] : meters_) voices.emplace(peer, meter->total());
    return voices;
}

std::map<PeerId, PlaybackStats> VoiceMeter::take_intervals() {
    std::lock_guard lock(mutex_);
    std::map<PeerId, PlaybackStats> voices;
    for (const auto& [peer, meter] : meters_) voices.emplace(peer, meter->take_interval());
    return voices;
}

PlaybackStats VoiceMeter::together(const std::map<PeerId, PlaybackStats>& voices) {
    PlaybackStats all;
    for (const auto& [peer, voice] : voices) {
        all.pictures += voice.pictures;
        if (voice.first_picture) {
            all.first_picture =
                std::max(all.first_picture.value_or(Duration{}), *voice.first_picture);
        }
        all.stalls += voice.stalls;
        all.stalled += voice.stalled;
        all.longest_gap = std::max(all.longest_gap, voice.longest_gap);
        all.latency.add(voice.latency);
    }
    return all;
}

}  // namespace ratcast
