#include "ratcast/engine/switchable_output.h"

#include <utility>

namespace ratcast {

SwitchableOutput::SwitchableOutput(std::unique_ptr<AudioOutput> device)
    : format_(device->format()), device_(std::move(device)) {}

SwitchableOutput::~SwitchableOutput() { stop(); }

Result<void> SwitchableOutput::start(Pull pull, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (started_) return fail(Errc::InvalidArgument);
    if (Result<void> started = device_->start(pull, ended); !started) return started;
    pull_ = std::move(pull);
    ended_ = std::move(ended);
    started_ = true;
    return {};
}

void SwitchableOutput::stop() {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    device_->stop();
    pull_ = {};
    ended_ = {};
    started_ = false;
}

Result<void> SwitchableOutput::switch_to(std::unique_ptr<AudioOutput> device) {
    if (!device || device->format() != format_) return fail(Errc::InvalidArgument);
    std::lock_guard lock(mutex_);
    if (started_) {
        // One device pulls at a time: the pull is no one's to share.
        device_->stop();
        if (Result<void> started = device->start(pull_, ended_); !started) {
            (void)device_->start(pull_, ended_);
            return started;
        }
    }
    device_ = std::move(device);
    return {};
}

}  // namespace ratcast
