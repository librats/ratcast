// Video encoding through MediaCodec: see mediacodec_encoder.h.

#include "android/mediacodec_encoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "android/mediacodec.h"
#include "ratcast/engine/video_convert.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

/// MediaCodec's BUFFER_FLAG_KEY_FRAME, which the NDK names only from API 34.
constexpr uint32_t kKeyFrameFlag = 1;
/// MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR.
constexpr int32_t kConstantBitrate = 2;
/// MediaCodecInfo.CodecProfileLevel: AVCProfileHigh, HEVCProfileMain.
constexpr int32_t kAvcHigh = 8;
constexpr int32_t kHevcMain = 1;
/// MediaFormat's COLOR_STANDARD_BT709, COLOR_STANDARD_BT601_PAL (BT.601's
/// matrix), COLOR_TRANSFER_SDR_VIDEO, COLOR_RANGE_FULL and _LIMITED.
constexpr int32_t kStandardBt709 = 1;
constexpr int32_t kStandardBt601 = 2;
constexpr int32_t kTransferSdr = 3;
constexpr int32_t kRangeFull = 1;
constexpr int32_t kRangeLimited = 2;
/// The keyframe interval, in seconds, when keyframes come only on request:
/// longer than any session.
constexpr int32_t kNoPeriodicKeyframes = 24 * 3600;

/// How long encode() waits for the frame of the picture it took: a hardware
/// encoder takes a few milliseconds, longer for its first.
constexpr int64_t kOutputWaitUs = 100'000;
constexpr int64_t kFirstOutputWaitUs = 1'000'000;
/// How long it waits for room for a picture.
constexpr int64_t kInputWaitUs = 50'000;

/// One way to set an encoder up; the first the encoder takes is used.
struct Setup {
    int32_t color_format;
    bool profile;  ///< H.264's High or HEVC's Main asked for, not left to the encoder
    bool constant_bitrate;
};

constexpr Setup kSetups[] = {
    {kColorFormatSemiPlanar, true, true},   {kColorFormatFlexible, true, true},
    {kColorFormatSemiPlanar, false, true},  {kColorFormatFlexible, false, true},
    {kColorFormatSemiPlanar, false, false}, {kColorFormatFlexible, false, false},
};

class MediaCodecEncoder final : public VideoEncoder {
public:
    MediaCodecEncoder(const VideoEncoderConfig& config, std::string name)
        : config_(config),
          name_(std::move(name)),
          info_{config.codec == Codec::H265 ? "hevc_mediacodec" : "h264_mediacodec", config.codec,
                true, PixelFormat::NV12, false} {}

    ~MediaCodecEncoder() override { close(); }

    /// The encoder made and started, in the first setup it takes.
    Result<void> open() {
        for (const Setup& setup : kSetups) {
            if (start(setup)) {
                LOG_DEBUG("engine", name_ << ": colour format " << setup.color_format
                                          << (setup.profile ? ", profile asked" : "")
                                          << (setup.constant_bitrate ? ", constant bitrate" : ""));
                return {};
            }
        }
        LOG_DEBUG("engine", name_ << " will not take " << config_.width << "x" << config_.height);
        return fail(Errc::Unavailable);
    }

    const VideoEncoderInfo& info() const noexcept override { return info_; }

    Result<void> encode(const VideoFrame& picture, bool keyframe, Sink sink) override {
        if ((picture.format() != PixelFormat::NV12 && picture.format() != PixelFormat::I420) ||
            picture.width() != config_.width || picture.height() != config_.height ||
            picture.color_space() != config_.color_space) {
            return fail(Errc::InvalidArgument);
        }
        if (picture.gpu()) {
            Result<VideoFrame> converted = convert_frame(picture, PixelFormat::NV12);
            if (!converted) return fail(converted.error());
            return encode(*converted, keyframe, sink);
        }
        if (!codec_) {  // lost with the one before: another, starting with a keyframe
            if (Result<void> opened = open(); !opened) return opened;
            keyframe_owed_ = true;
        }

        ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 0);
        if (index < 0) {
            // Every buffer taken: what is done comes out first, making room.
            if (Result<void> drained = drain(sink, 0); !drained) return drained;
            index = AMediaCodec_dequeueInputBuffer(codec_, kInputWaitUs);
        }
        if (index < 0) {
            LOG_WARN("engine", name_ << " takes no more pictures: " << index);
            return fail(Errc::Internal);
        }
        size_t capacity = 0;
        uint8_t* input = AMediaCodec_getInputBuffer(codec_, size_t(index), &capacity);
        if (!input || !layout_.fits(capacity)) {
            AMediaCodec_queueInputBuffer(codec_, size_t(index), 0, 0, 0, 0);
            LOG_WARN("engine", name_ << "'s buffer is smaller than its pictures");
            close();
            return fail(Errc::Internal);
        }
        const size_t size = copy_in(picture, input, capacity);

        if (std::exchange(keyframe_owed_, false) || keyframe) set_parameter("request-sync", 0);
        // Encoders insist on increasing times; two pictures stamped alike
        // are a microsecond apart.
        int64_t pts = picture.capture_time().time_since_epoch().count();
        if (last_pts_ && pts <= *last_pts_) pts = *last_pts_ + 1;
        last_pts_ = pts;
        if (const media_status_t queued =
                AMediaCodec_queueInputBuffer(codec_, size_t(index), 0, size, uint64_t(pts), 0);
            queued != AMEDIA_OK) {
            LOG_WARN("engine", name_ << " refused a picture: " << queued);
            close();
            return fail(Errc::Internal);
        }
        ++pending_;
        return drain(sink, frames_ > 0 ? kOutputWaitUs : kFirstOutputWaitUs);
    }

    void set_bitrate(uint32_t bits_per_second) override {
        if (bits_per_second == 0 || bits_per_second == config_.bitrate) return;
        config_.bitrate = bits_per_second;
        if (codec_ && !set_parameter("video-bitrate", int32_t(bits_per_second))) {
            LOG_WARN("engine",
                     name_ << " kept its bitrate: " << bits_per_second << " bit/s refused");
        }
    }

    // Frames still owed come out; the next picture is a keyframe, as the
    // interface promises.
    Result<void> flush(Sink sink) override {
        keyframe_owed_ = true;
        if (!codec_) return {};
        return drain(sink, kOutputWaitUs);
    }

private:
    bool start(const Setup& setup) {
        codec_ = AMediaCodec_createCodecByName(name_.c_str());
        if (!codec_) return false;
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime_of(config_.codec));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, int32_t(config_.width));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, int32_t(config_.height));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, setup.color_format);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(config_.bitrate));
        if (setup.constant_bitrate) AMediaFormat_setInt32(format, "bitrate-mode", kConstantBitrate);
        const uint32_t rate = std::max<uint32_t>(
            1, config_.frame_rate_num / std::max<uint32_t>(1, config_.frame_rate_den));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(rate));
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL,
                              config_.keyframe_interval > 0
                                  ? int32_t(std::max<uint32_t>(1, config_.keyframe_interval / rate))
                                  : kNoPeriodicKeyframes);
        if (setup.profile) {
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PROFILE,
                                  config_.codec == Codec::H265 ? kHevcMain : kAvcHigh);
        }
        AMediaFormat_setInt32(format, "max-bframes", 0);
        AMediaFormat_setInt32(format, "latency", 1);  // Android 11's KEY_LATENCY
        AMediaFormat_setInt32(format, "vendor.qti-ext-enc-low-latency.enable", 1);
        AMediaFormat_setInt32(format, "priority", 0);  // real time
        AMediaFormat_setInt32(
            format, AMEDIAFORMAT_KEY_COLOR_STANDARD,
            config_.color_space.matrix == ColorMatrix::BT601 ? kStandardBt601 : kStandardBt709);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_TRANSFER, kTransferSdr);
        AMediaFormat_setInt32(
            format, AMEDIAFORMAT_KEY_COLOR_RANGE,
            config_.color_space.range == ColorRange::Full ? kRangeFull : kRangeLimited);
        media_status_t status = AMediaCodec_configure(codec_, format, nullptr, nullptr,
                                                      AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
        AMediaFormat_delete(format);
        if (status == AMEDIA_OK) status = AMediaCodec_start(codec_);
        if (status == AMEDIA_OK) {
            if (AMediaFormat* input = AMediaCodec_getInputFormat(codec_)) {
                layout_ = layout_of(input);
                AMediaFormat_delete(input);
            }
            // Every picture covered: a layout with no size of its own is the config's.
            if (layout_.width == 0 || layout_.height == 0) {
                layout_.width = config_.width;
                layout_.height = config_.height;
            }
            if (layout_.known && layout_.width >= config_.width &&
                layout_.height >= config_.height) {
                layout_.width = config_.width;
                layout_.height = config_.height;
                info_.input_format = layout_.nv12() ? PixelFormat::NV12 : PixelFormat::I420;
                return true;
            }
            LOG_DEBUG("engine", name_ << " takes pictures in a layout the engine does not write");
        }
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        return false;
    }

    /// `picture` (NV12 or I420) into the encoder's `input`, laid out as it
    /// asks; the bytes its picture takes up there.
    size_t copy_in(const VideoFrame& picture, uint8_t* input, size_t capacity) const {
        const MediaLayout& layout = layout_;
        const uint32_t width = config_.width;
        const uint32_t height = config_.height;
        const MediaLayout::Plane& y = layout.planes[0];
        for (uint32_t row = 0; row < height; ++row) {
            const uint8_t* from = picture.plane(0).data + row * picture.plane(0).stride;
            uint8_t* to = input + y.offset + row * size_t(y.row_step);
            if (y.column_step == 1) {
                std::memcpy(to, from, width);
                continue;
            }
            for (uint32_t column = 0; column < width; ++column)
                to[column * size_t(y.column_step)] = from[column];
        }
        const MediaLayout::Plane& u = layout.planes[1];
        const MediaLayout::Plane& v = layout.planes[2];
        const bool nv12_in = picture.format() == PixelFormat::NV12;
        for (uint32_t row = 0; row < height / 2; ++row) {
            uint8_t* to_u = input + u.offset + row * size_t(u.row_step);
            uint8_t* to_v = input + v.offset + row * size_t(v.row_step);
            const uint8_t* from = picture.plane(1).data + row * picture.plane(1).stride;
            if (nv12_in && layout.nv12()) {
                std::memcpy(to_u, from, width);
                continue;
            }
            const uint8_t* from_v =
                nv12_in ? from + 1 : picture.plane(2).data + row * picture.plane(2).stride;
            const size_t step = nv12_in ? 2 : 1;
            for (uint32_t column = 0; column < width / 2; ++column) {
                to_u[column * size_t(u.column_step)] = from[column * step];
                to_v[column * size_t(v.column_step)] = from_v[column * step];
            }
        }
        // Up to the end of each plane's last whole row.
        size_t end = 0;
        for (size_t i = 0; i < 3; ++i) {
            const size_t rows = i == 0 ? height : height / 2;
            end = std::max(end, layout.planes[i].offset + rows * size_t(layout.planes[i].row_step));
        }
        return std::min(end, capacity);
    }

    /// The frames done, the first waited for `wait_us` while one is owed.
    Result<void> drain(Sink sink, int64_t wait_us) {
        for (;;) {
            AMediaCodecBufferInfo buffer{};
            const ssize_t index =
                AMediaCodec_dequeueOutputBuffer(codec_, &buffer, pending_ > 0 ? wait_us : 0);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                if (pending_ > 0 && wait_us > 0 && !late_said_) {
                    late_said_ = true;
                    LOG_WARN("engine", name_ << " holds pictures back: frames come out late");
                }
                return {};
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
                index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;
            }
            if (index < 0) {
                LOG_WARN("engine", name_ << " failed: " << index << ", opened again");
                close();
                return fail(Errc::Internal);
            }
            size_t capacity = 0;
            const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, size_t(index), &capacity);
            const bool whole =
                data && buffer.size > 0 && size_t(buffer.offset) + size_t(buffer.size) <= capacity;
            const std::span<const uint8_t> bytes =
                whole ? std::span<const uint8_t>(data + buffer.offset, size_t(buffer.size))
                      : std::span<const uint8_t>();
            if (buffer.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                // The parameter sets, ahead of the first frame: put ahead of
                // every keyframe.
                parameter_sets_.assign(bytes.begin(), bytes.end());
                AMediaCodec_releaseOutputBuffer(codec_, size_t(index), false);
                continue;
            }
            pending_ = std::max(0, pending_ - 1);
            wait_us = 0;
            if (!bytes.empty())
                sink(frame_of(bytes, (buffer.flags & kKeyFrameFlag) != 0,
                              buffer.presentationTimeUs));
            AMediaCodec_releaseOutputBuffer(codec_, size_t(index), false);
        }
    }

    /// One frame, its parameter sets ahead of it if a keyframe without them.
    EncodedVideo frame_of(std::span<const uint8_t> bytes, bool keyframe, int64_t pts) {
        EncodedVideo out;
        out.info.keyframe = keyframe;
        const bool sets =
            keyframe && !parameter_sets_.empty() &&
            (bytes.size() < parameter_sets_.size() ||
             !std::equal(parameter_sets_.begin(), parameter_sets_.end(), bytes.begin()));
        out.payload = allocate_frame_payload((sets ? parameter_sets_.size() : 0) + bytes.size());
        if (sets) out.payload.append(parameter_sets_);
        out.payload.append(bytes);
        out.info.number = next_number_++;
        out.info.capture_time = Timestamp{Duration{pts}};
        // One chain from keyframe to keyframe: each frame needs the one
        // before (and through it everything back to the keyframe).
        if (!keyframe && out.info.number > 0) out.info.references = {out.info.number - 1};
        ++frames_;
        return out;
    }

    bool set_parameter(const char* key, int32_t value) {
        AMediaFormat* parameters = AMediaFormat_new();
        AMediaFormat_setInt32(parameters, key, value);
        const media_status_t status = AMediaCodec_setParameters(codec_, parameters);
        AMediaFormat_delete(parameters);
        return status == AMEDIA_OK;
    }

    /// Let the encoder go, and whatever it still had: the next picture opens
    /// another.
    void close() {
        if (!codec_) return;
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        pending_ = 0;
        parameter_sets_.clear();
    }

    VideoEncoderConfig config_;
    const std::string name_;  ///< MediaCodec's
    VideoEncoderInfo info_;
    AMediaCodec* codec_ = nullptr;
    MediaLayout layout_;
    std::vector<uint8_t> parameter_sets_;
    /// The next picture is a keyframe whether asked for or not.
    bool keyframe_owed_ = false;
    int pending_ = 0;  ///< pictures given whose frames have not come
    uint64_t frames_ = 0;
    bool late_said_ = false;
    std::optional<int64_t> last_pts_;
    FrameNumber next_number_ = 0;
};

}  // namespace

Result<std::unique_ptr<VideoEncoder>> open_mediacodec_encoder(const VideoEncoderConfig& config) {
    if (config.codec != Codec::H264 && config.codec != Codec::H265) return fail(Errc::Unavailable);
    AMediaCodec* found = AMediaCodec_createEncoderByType(mime_of(config.codec));
    if (!found) return fail(Errc::Unavailable);
    const std::string name = name_of(found);
    AMediaCodec_delete(found);
    if (name.empty() || is_software(name)) return fail(Errc::Unavailable);
    auto encoder = std::make_unique<MediaCodecEncoder>(config, name);
    if (Result<void> opened = encoder->open(); !opened) return fail(opened.error());
    LOG_DEBUG("engine", "MediaCodec's " << name);
    return std::unique_ptr<VideoEncoder>(std::move(encoder));
}

}  // namespace ratcast::android
