#pragma once

// How far apart an encoder's pictures come, on average. An encoder that
// spends bitrate / frame rate on each picture, whatever its timestamps say,
// sends a film of 24 pictures a second shared at 60 at 24/60 of its bitrate:
// it is told as much more as pictures come further apart than its frame rate
// (told_bitrate()). FFmpeg's encoders of the table that do so
// (ffmpeg/video_encoder.cpp) and VideoToolbox's (macos/videotoolbox_encoder.cpp).

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>

#include "ratcast/core/time.h"

namespace ratcast {

class PictureSpacing {
public:
    /// At most this many times (or as many times less, nearer): a still
    /// screen's next picture carries up to its encoder's buffer of frames
    /// times this over the frame rate of a second's bits.
    static constexpr double kMostApart = 3;
    /// Averaged over about this many pictures as they come further apart, and
    /// over this few as they come nearer: spending more than the path carries
    /// fills its queue, spending less only costs some quality.
    static constexpr double kSpacingOver = 16;
    static constexpr double kNearerOver = 4;

    /// For pictures meant to come `frame_rate_num / frame_rate_den` a second.
    PictureSpacing(uint32_t frame_rate_num, uint32_t frame_rate_den) noexcept
        : nominal_(1e6 * frame_rate_den / std::max<uint32_t>(frame_rate_num, 1)),
          spacing_(nominal_) {}

    /// A picture taken at `capture_time`.
    void add(Timestamp capture_time) noexcept {
        if (last_) {
            const double apart = std::clamp(double((capture_time - *last_).count()),
                                            nominal_ / kMostApart, nominal_ * kMostApart);
            spacing_ += (apart - spacing_) / (apart < spacing_ ? kNearerOver : kSpacingOver);
        }
        last_ = capture_time;
    }

    /// How far apart pictures come on average, in µs.
    double average() const noexcept { return spacing_; }

    /// What to tell an encoder that spends by frame rate for it to spend
    /// `bitrate` a second over the pictures as they come.
    uint32_t told_bitrate(uint32_t bitrate) const noexcept {
        return static_cast<uint32_t>(std::min(double(bitrate) * spacing_ / nominal_,
                                              double(std::numeric_limits<uint32_t>::max())));
    }

private:
    double nominal_;  ///< µs apart at the frame rate
    double spacing_;  ///< µs apart on average
    std::optional<Timestamp> last_;
};

}  // namespace ratcast
