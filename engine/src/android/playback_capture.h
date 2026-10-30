#pragma once

/**
 * @file playback_capture.h
 * @brief What other applications play on Android: AudioPlaybackCapture.
 *
 * Only a MediaProjection's AudioRecord hears it, so the capture is of the
 * projection the screen's capture holds (android/projection.h): opened beside
 * the screen, it starts while that is shared, and fails with Unavailable
 * otherwise. Media, games and sound that says nothing of its use are heard;
 * never a call's voices (voice communication's use), nor this process's
 * sound, nor that of an application that does not allow it. Blocks of 10 ms
 * are read on a thread of the capture's own, stamped as they come in.
 * Needs RECORD_AUDIO.
 */

#include <jni.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "ratcast/core/time.h"
#include "ratcast/engine/audio_device.h"

namespace ratcast::android {

class Projection;

class PlaybackCapture final : public AudioSource {
public:
    explicit PlaybackCapture(const AudioCaptureOptions& options,
                             Clock& clock = SteadyClock::instance());
    ~PlaybackCapture() override;

    const AudioFormat& format() const noexcept override { return format_; }

    /// Fails with Unavailable when no screen is shared, or no AudioRecord
    /// will open (no RECORD_AUDIO).
    Result<void> start(Sink sink, EndHandler ended) override;
    void stop() override;

private:
    void read(Sink sink, EndHandler ended);

    const AudioFormat format_;
    Clock& clock_;
    std::mutex mutex_;  ///< serialises start() and stop()
    std::shared_ptr<Projection> projection_;
    jobject record_ = nullptr;  ///< the AudioRecord, a global reference
    std::atomic<bool> stopping_{false};
    std::thread reader_;
};

}  // namespace ratcast::android
