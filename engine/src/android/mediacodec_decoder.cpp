// Video decoding through MediaCodec: see mediacodec_decoder.h.

#include "android/mediacodec_decoder.h"

#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "android/hardware_picture.h"
#include "android/mediacodec.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

/// How long decode() waits for the picture of the frame it was given: a
/// hardware decoder takes a few milliseconds for 4K, and longer for its
/// first, as it sets itself up.
constexpr int64_t kOutputWaitUs = 30'000;
constexpr int64_t kFirstOutputWaitUs = 500'000;
/// How long it waits for room for a frame, the decoder's pictures taken out
/// meanwhile.
constexpr int64_t kInputWaitUs = 50'000;
/// How long a picture rendered into the reader's surface is waited for there.
constexpr auto kImageWait = std::chrono::milliseconds(30);
/// How often the reader is looked at meanwhile.
constexpr auto kImageLookEvery = std::chrono::microseconds(500);
/// Pictures the reader of GPU pictures holds, those a view keeps among them:
/// the one drawn, the one waiting, those the GPU may still be drawing from.
constexpr int32_t kHeldImages = 8;
/// What GPU pictures are decoded for: the GPU draws them, and the CPU may
/// read them for whatever does not.
constexpr uint64_t kGpuUsage =
    AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

/// A reader, deleted once neither the decoder nor a picture of its holds it.
using Reader = std::shared_ptr<AImageReader>;

/// The matrix of MediaFormat's COLOR_STANDARD: BT.601's 2 to 5 — or, past
/// 64, the pair of primaries and matrix no standard names, as Android's
/// ColorUtils wraps it (64 + primaries + 7 x matrix): BT.709's primaries and
/// BT.601's matrix, which the engine's encoders signal for SD, come as 86.
ColorMatrix matrix_of(int32_t standard) {
    if (standard >= 64) {
        const int32_t matrix = (standard - 64) / 7;                    // ColorAspects::MatrixCoeffs
        return matrix == 3 ? ColorMatrix::BT601 : ColorMatrix::BT709;  // MatrixBT601_6
    }
    return standard >= 2 && standard <= 5 ? ColorMatrix::BT601 : ColorMatrix::BT709;
}

int32_t int_of(AMediaFormat* format, const char* key, int32_t otherwise) {
    int32_t value = 0;
    return AMediaFormat_getInt32(format, key, &value) ? value : otherwise;
}

class MediaCodecDecoder final : public VideoDecoder {
public:
    /// With `reader`, the decoder renders into its surface: pictures in GPU
    /// memory.
    MediaCodecDecoder(AMediaCodec* codec, std::string name, Reader reader)
        : codec_(codec), info_{std::move(name)}, reader_(std::move(reader)) {}

    ~MediaCodecDecoder() override {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
    }

    const VideoDecoderInfo& info() const noexcept override { return info_; }

    Result<void> decode(const EncodedFrame& frame, Sink sink) override {
        ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 0);
        if (index < 0) {
            // Every buffer taken: what is done comes out first, making room.
            if (Result<void> drained = drain(sink, 0); !drained) return drained;
            index = AMediaCodec_dequeueInputBuffer(codec_, kInputWaitUs);
        }
        if (index < 0) {
            LOG_WARN("engine", info_.name << " takes no more frames: started again");
            restart();
            return fail(Errc::Malformed);
        }
        size_t capacity = 0;
        uint8_t* input = AMediaCodec_getInputBuffer(codec_, size_t(index), &capacity);
        const size_t size = frame.payload.size();
        if (!input || size > capacity) {
            AMediaCodec_queueInputBuffer(codec_, size_t(index), 0, 0, 0, 0);
            return fail(Errc::Malformed);
        }
        std::memcpy(input, frame.payload.data(), size);
        const uint64_t pts = uint64_t(frame.info.capture_time.time_since_epoch().count());
        if (AMediaCodec_queueInputBuffer(codec_, size_t(index), 0, size, pts, 0) != AMEDIA_OK) {
            restart();
            return fail(Errc::Malformed);
        }
        ++pending_;
        return drain(sink, pictures_ > 0 ? kOutputWaitUs : kFirstOutputWaitUs);
    }

private:
    /// The pictures done, the first waited for `wait_us` while one is owed.
    Result<void> drain(Sink sink, int64_t wait_us) {
        for (;;) {
            AMediaCodecBufferInfo buffer{};
            const ssize_t index =
                AMediaCodec_dequeueOutputBuffer(codec_, &buffer, pending_ > 0 ? wait_us : 0);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                // Waited for in vain: the decoder holds that frame back, or
                // made nothing of it. Not waited for again.
                if (pending_ > 0 && wait_us > 0) pending_ = 0;
                return {};
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                read_format();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (index < 0) {
                LOG_WARN("engine", info_.name << " failed: " << index << ", started again");
                restart();
                return fail(Errc::Malformed);
            }
            pending_ = std::max(0, pending_ - 1);
            ++pictures_;
            wait_us = 0;
            if (reader_) {
                // Into the reader, stamped with the frame's time.
                AMediaCodec_releaseOutputBuffer(codec_, size_t(index), buffer.size > 0);
                if (buffer.size > 0) ++rendering_;
                collect(sink);
                continue;
            }
            size_t capacity = 0;
            const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, size_t(index), &capacity);
            Result<void> taken;
            if (data && buffer.size > 0 && size_t(buffer.offset) + size_t(buffer.size) <= capacity)
                taken = take(data + buffer.offset, size_t(buffer.size), buffer.presentationTimeUs,
                             sink);
            AMediaCodec_releaseOutputBuffer(codec_, size_t(index), false);
            if (!taken) return taken;
        }
    }

    /// The pictures rendered into the reader, each handed out in its buffer
    /// as it comes there, those still owed waited for a little.
    void collect(Sink sink) {
        const auto until = std::chrono::steady_clock::now() + kImageWait;
        for (;;) {
            AImage* acquired = nullptr;
            const media_status_t status = AImageReader_acquireNextImage(reader_.get(), &acquired);
            if (status == AMEDIA_OK && acquired) {
                rendering_ = std::max(0, rendering_ - 1);
                hand_out(acquired, sink);
                continue;
            }
            if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED && !full_said_) {
                full_said_ = true;
                LOG_WARN("engine", info_.name << ": pictures kept too long, others wait");
            }
            if (rendering_ == 0 || status != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE ||
                std::chrono::steady_clock::now() >= until) {
                rendering_ = 0;  // come later, if at all: taken with the next
                return;
            }
            std::this_thread::sleep_for(kImageLookEvery);
        }
    }

    /// The picture of `acquired`, held with the image and its reader.
    void hand_out(AImage* acquired, Sink sink) {
        std::shared_ptr<const void> image(acquired, [reader = reader_](const void* held) {
            AImage_delete(static_cast<AImage*>(const_cast<void*>(held)));
        });
        int64_t time_ns = 0;
        if (AImage_getTimestamp(acquired, &time_ns) != AMEDIA_OK) return;
        VideoFrame picture = gpu_frame_of(
            acquired, std::move(image),
            Timestamp{std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(time_ns))},
            color_space_);
        if (!picture.empty()) {
            sink(std::move(picture));
        } else if (!crop_said_) {
            crop_said_ = true;
            LOG_WARN("engine", info_.name << " crops its pictures elsewhere than the top left");
        }
    }

    /// Where the planes of the decoder's pictures are, and what they show.
    void read_format() {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
        if (!format) return;
        layout_ = layout_of(format);
        if (!layout_.known && !reader_)
            LOG_WARN("engine",
                     info_.name << " gives pictures in a layout the engine does not read");

        // MediaFormat's COLOR_RANGE_FULL is 1.
        const int32_t range = int_of(format, AMEDIAFORMAT_KEY_COLOR_RANGE, 2);
        color_space_ = {matrix_of(int_of(format, AMEDIAFORMAT_KEY_COLOR_STANDARD, 1)),
                        range == 1 ? ColorRange::Full : ColorRange::Limited};
        AMediaFormat_delete(format);
    }

    /// A picture copied out of the decoder's buffer into a pooled NV12 frame.
    Result<void> take(const uint8_t* data, size_t size, int64_t pts, Sink sink) {
        if (!layout_.known || layout_.width == 0 || layout_.height == 0)
            return fail(Errc::Unsupported);
        const MediaLayout& layout = layout_;
        if (!layout.fits(size)) return fail(Errc::Malformed);

        MutableVideoFrame picture =
            MutableVideoFrame::allocate(PixelFormat::NV12, layout.width, layout.height);
        const MediaLayout::Plane& y = layout.planes[0];
        const uint8_t* from = data + y.offset + layout.top * size_t(y.row_step) + layout.left;
        for (uint32_t row = 0; row < layout.height; ++row) {
            std::memcpy(picture.plane(0).data + row * picture.plane(0).stride,
                        from + row * size_t(y.row_step), layout.width);
        }
        const MediaLayout::Plane& u = layout.planes[1];
        const MediaLayout::Plane& v = layout.planes[2];
        const size_t chroma_left = layout.left / 2;
        const size_t chroma_top = layout.top / 2;
        const bool nv12 = layout.nv12();
        for (uint32_t row = 0; row < layout.height / 2; ++row) {
            uint8_t* to = picture.plane(1).data + row * picture.plane(1).stride;
            const uint8_t* from_u = data + u.offset + (chroma_top + row) * size_t(u.row_step) +
                                    chroma_left * size_t(u.column_step);
            if (nv12) {
                std::memcpy(to, from_u, layout.width);
                continue;
            }
            const uint8_t* from_v = data + v.offset + (chroma_top + row) * size_t(v.row_step) +
                                    chroma_left * size_t(v.column_step);
            for (uint32_t column = 0; column < layout.width / 2; ++column) {
                to[2 * column] = from_u[column * size_t(u.column_step)];
                to[2 * column + 1] = from_v[column * size_t(v.column_step)];
            }
        }
        sink(std::move(picture).freeze(Timestamp{Duration{pts}}, color_space_));
        return {};
    }

    /// After a failure: the decoder emptied, to start again at a keyframe.
    void restart() {
        AMediaCodec_flush(codec_);
        pending_ = 0;
    }

    AMediaCodec* const codec_;
    VideoDecoderInfo info_;
    const Reader reader_;  ///< what pictures are rendered into, if anything
    MediaLayout layout_;
    ColorSpace color_space_;
    int pending_ = 0;    ///< frames given whose pictures have not come
    int rendering_ = 0;  ///< pictures rendered that have not reached the reader
    uint64_t pictures_ = 0;
    bool full_said_ = false;
    bool crop_said_ = false;
};

/// A reader of GPU pictures for a decoder to render into, or null.
Reader gpu_reader() {
    AImageReader* made = nullptr;
    // The size is the decoder's to say, buffer by buffer.
    if (AImageReader_newWithUsage(1920, 1080, AIMAGE_FORMAT_YUV_420_888, kGpuUsage, kHeldImages,
                                  &made) != AMEDIA_OK) {
        return nullptr;
    }
    return Reader(made, AImageReader_delete);
}

/// The name of the decoder the device picks for `mime`, made to be asked.
std::string default_decoder(const char* mime) {
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) return {};
    std::string name = name_of(codec);
    AMediaCodec_delete(codec);
    return name;
}

}  // namespace

std::vector<Codec> hardware_video_decoders() {
    std::vector<Codec> codecs;
    for (const Codec codec : {Codec::AV1, Codec::H265, Codec::H264}) {
        const std::string name = default_decoder(mime_of(codec));
        if (!name.empty() && !is_software(name)) codecs.push_back(codec);
    }
    return codecs;
}

Result<std::unique_ptr<VideoDecoder>> open_mediacodec_decoder(const VideoDecoderConfig& config) {
    const char* mime = mime_of(config.codec);
    if (!mime) return fail(Errc::Unavailable);
    AMediaCodec* codec = AMediaCodec_createDecoderByType(mime);
    if (!codec) return fail(Errc::Unavailable);
    std::string name = name_of(codec);
    // Qualcomm's low-latency component of the same decoder, where there is one.
    if (!name.empty() && !is_software(name)) {
        const std::string low_latency = name + ".low_latency";
        if (AMediaCodec* faster = AMediaCodec_createCodecByName(low_latency.c_str())) {
            AMediaCodec_delete(codec);
            codec = faster;
            name = low_latency;
        }
    }
    if (name.empty() || is_software(name)) {
        AMediaCodec_delete(codec);
        return fail(Errc::Unavailable);
    }

    // The size is the stream's to say; this one only sizes the first buffers.
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, 1920);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, 1080);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorFormatFlexible);
    AMediaFormat_setInt32(format, "low-latency", 1);  // Android 11's KEY_LOW_LATENCY
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
    // Pictures out in the order they are decoded: the engine's encoders make
    // no B-frames, so none is held back to be put in order.
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(format, "priority", 0);  // real time
    if (!config.codec_config.empty()) {
        AMediaFormat_setBuffer(format, "csd-0", config.codec_config.data(),
                               config.codec_config.size());
    }
    // GPU pictures asked for: rendered into a reader's surface, or, where
    // the decoder will not, in memory after all.
    Reader reader = config.gpu ? gpu_reader() : nullptr;
    ANativeWindow* window = nullptr;
    if (reader && AImageReader_getWindow(reader.get(), &window) != AMEDIA_OK) reader.reset();
    media_status_t status =
        AMediaCodec_configure(codec, format, reader ? window : nullptr, nullptr, 0);
    if (status != AMEDIA_OK && reader) {
        LOG_INFO("engine", "decoder " << name << " renders into no surface: " << status);
        reader.reset();
        AMediaCodec_delete(codec);
        codec = AMediaCodec_createCodecByName(name.c_str());
        status = codec ? AMediaCodec_configure(codec, format, nullptr, nullptr, 0)
                       : AMEDIA_ERROR_UNKNOWN;
    }
    AMediaFormat_delete(format);
    if (!codec) return fail(Errc::Unavailable);
    if (status == AMEDIA_OK) status = AMediaCodec_start(codec);
    if (status != AMEDIA_OK) {
        LOG_WARN("engine", "decoder " << name << " cannot open: " << status);
        AMediaCodec_delete(codec);
        return fail(Errc::Unavailable);
    }
    LOG_INFO("engine", "video decoder: " << name << (reader ? ", into GPU memory" : ""));
    return std::unique_ptr<VideoDecoder>(
        std::make_unique<MediaCodecDecoder>(codec, name, std::move(reader)));
}

}  // namespace ratcast::android
