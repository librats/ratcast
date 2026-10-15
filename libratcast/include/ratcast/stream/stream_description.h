#pragma once

/**
 * @file stream_description.h
 * @brief What a sender tells its peers about a stream before any frame of it.
 *
 * A receiver needs this to decide whether to subscribe ("screen" or "camera",
 * which codec, what size) and to set up a decoder before the first frame
 * arrives. It is announced again whenever it changes.
 *
 * Decoders should not depend on `codec_config` alone, though: the engine's
 * encoders repeat their parameter sets (SPS/PPS, the AV1 sequence header) in
 * every keyframe, so a keyframe is decodable on its own even if it overtakes
 * the announcement of a change.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "ratcast/core/buffer.h"
#include "ratcast/core/time.h"
#include "ratcast/export.h"
#include "ratcast/stream/frame.h"

namespace ratcast {

enum class MediaKind : uint8_t {
    Video = 1,
    Audio = 2,
};

/// The bitstream format of a stream's frames. libratcast never looks inside a
/// frame, so a value it does not name is carried through unchanged: a newer
/// sender can announce a codec an older receiver simply has no decoder for.
enum class Codec : uint8_t {
    H264 = 1,
    H265 = 2,
    AV1 = 3,
    Opus = 64,
};

/// The kind of media `codec` carries; nullopt for a codec this build does not
/// name.
RATCAST_EXPORT std::optional<MediaKind> media_kind_of(Codec codec) noexcept;

struct VideoFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    /// Nominal frames per second, as a fraction (30000/1001); 0/1 when the
    /// source has no fixed rate, as a screen that only sends what changes.
    uint32_t frame_rate_num = 0;
    uint32_t frame_rate_den = 1;

    friend bool operator==(const VideoFormat&, const VideoFormat&) = default;
};

struct AudioFormat {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;

    friend bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

struct StreamDescription {
    static constexpr size_t kMaxLabelSize = 64;
    static constexpr size_t kMaxCodecConfigSize = 64 * 1024;
    static constexpr uint32_t kMaxDimension = 16384;
    static constexpr Duration kMaxLifetime = std::chrono::seconds(30);

    StreamId id = 0;
    Codec codec = Codec::H264;
    std::variant<VideoFormat, AudioFormat> format;
    /// How long after it is sent a frame is still worth delivering — the
    /// latency profile of the stream. Past it a frame is dropped, not late.
    Duration lifetime{};
    /// What the stream is to the application ("screen", "camera", "mic"),
    /// at most kMaxLabelSize bytes.
    std::string label;
    /// Out-of-band decoder configuration (avcC, hvcC, av1C, OpusHead); may be
    /// empty. At most kMaxCodecConfigSize bytes.
    Buffer codec_config;

    MediaKind kind() const noexcept {
        return std::holds_alternative<VideoFormat>(format) ? MediaKind::Video : MediaKind::Audio;
    }
    const VideoFormat* video() const noexcept { return std::get_if<VideoFormat>(&format); }
    const AudioFormat* audio() const noexcept { return std::get_if<AudioFormat>(&format); }
};

RATCAST_EXPORT bool operator==(const StreamDescription& a, const StreamDescription& b) noexcept;

/// Whether `description` can be announced: a lifetime in (0, kMaxLifetime],
/// sizes within their limits, a format of the kind its codec carries, and
/// sane video dimensions and frame rate.
RATCAST_EXPORT bool is_valid(const StreamDescription& description) noexcept;

}  // namespace ratcast
