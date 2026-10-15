#include "ratcast/stream/stream_description.h"

#include <algorithm>

namespace ratcast {

std::optional<MediaKind> media_kind_of(Codec codec) noexcept {
    switch (codec) {
        case Codec::H264:
        case Codec::H265:
        case Codec::AV1: return MediaKind::Video;
        case Codec::Opus: return MediaKind::Audio;
    }
    return std::nullopt;
}

bool operator==(const StreamDescription& a, const StreamDescription& b) noexcept {
    return a.id == b.id && a.codec == b.codec && a.format == b.format && a.lifetime == b.lifetime &&
           a.label == b.label && std::ranges::equal(a.codec_config.bytes(), b.codec_config.bytes());
}

bool is_valid(const StreamDescription& description) noexcept {
    if (description.lifetime <= Duration::zero()) return false;
    if (description.lifetime > StreamDescription::kMaxLifetime) return false;
    if (description.label.size() > StreamDescription::kMaxLabelSize) return false;
    if (description.codec_config.size() > StreamDescription::kMaxCodecConfigSize) return false;

    const std::optional<MediaKind> kind = media_kind_of(description.codec);
    if (kind && *kind != description.kind()) return false;

    if (const VideoFormat* video = description.video()) {
        if (video->width == 0 || video->width > StreamDescription::kMaxDimension) return false;
        if (video->height == 0 || video->height > StreamDescription::kMaxDimension) return false;
        if (video->frame_rate_den == 0) return false;
    }
    if (const AudioFormat* audio = description.audio()) {
        if (audio->sample_rate == 0 || audio->channels == 0) return false;
    }
    return true;
}

}  // namespace ratcast
