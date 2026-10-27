// VideoToolbox's compression session, one picture at a time: each is copied
// into a buffer of the session's pool, encoded, and waited for
// (VTCompressionSessionCompleteFrames), so its frame is handed out by the call
// that took it. Frames leave in Annex B, a keyframe's parameter sets ahead of
// it, as FFmpeg's encoders give them and its decoders take them.

#include "macos/videotoolbox_encoder.h"

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "ratcast/engine/video_convert.h"
#include "ratcast/stream/frame.h"

#include "librats/util/logger.h"

namespace ratcast::macos {
namespace {

struct CfRelease {
    void operator()(const void* ref) const noexcept { CFRelease(ref); }
};

/// A Core Foundation reference of one's own, released with it.
template <typename Ref>
using CfPtr = std::unique_ptr<std::remove_pointer_t<Ref>, CfRelease>;

CfPtr<CFNumberRef> number(int64_t value) {
    return CfPtr<CFNumberRef>(CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &value));
}

CfPtr<CFNumberRef> number(double value) {
    return CfPtr<CFNumberRef>(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &value));
}

CfPtr<CFMutableDictionaryRef> dictionary() {
    return CfPtr<CFMutableDictionaryRef>(CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

/// MaxKeyFrameInterval when keyframes come only on request: longer than any
/// session.
constexpr int64_t kNoPeriodicKeyframes = 1 << 30;

/// The timescale of the session's times: capture times' microseconds.
constexpr int32_t kMicroseconds = 1'000'000;

/// What each NAL unit starts with in Annex B, in place of its length.
constexpr uint8_t kStartCode[] = {0, 0, 0, 1};

/// Calls `unit` with each NAL unit of `data`, each after a big-endian length
/// of `length_size` bytes; false if one runs past the end.
template <typename Unit>
bool for_each_unit(std::span<const uint8_t> data, size_t length_size, Unit unit) {
    while (!data.empty()) {
        if (data.size() < length_size) return false;
        size_t length = 0;
        for (size_t i = 0; i < length_size; ++i) length = length << 8 | data[i];
        data = data.subspan(length_size);
        if (length > data.size()) return false;
        unit(data.first(length));
        data = data.subspan(length);
    }
    return true;
}

bool is_keyframe(CMSampleBufferRef sample) {
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return true;
    const auto* first = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
    const auto* not_sync =
        static_cast<CFBooleanRef>(CFDictionaryGetValue(first, kCMSampleAttachmentKey_NotSync));
    return !not_sync || !CFBooleanGetValue(not_sync);
}

class VideoToolboxEncoder final : public VideoEncoder {
public:
    explicit VideoToolboxEncoder(const VideoEncoderConfig& config)
        : config_(config),
          info_{config.codec == Codec::H265 ? "hevc_videotoolbox" : "h264_videotoolbox",
                config.codec, true, PixelFormat::NV12, false} {}

    ~VideoToolboxEncoder() override { close(); }

    /// A session for the config: with low-latency rate control where the
    /// encoder has it (H.264 on Apple's GPUs), else without.
    Result<void> open() {
        if (config_.codec == Codec::H264 && create(true)) return {};
        return create(false);
    }

    const VideoEncoderInfo& info() const noexcept override { return info_; }

    Result<void> encode(const VideoFrame& picture, bool keyframe, Sink sink) override {
        if (picture.format() == PixelFormat::BGRA || picture.width() != config_.width ||
            picture.height() != config_.height || picture.color_space() != config_.color_space) {
            return fail(Errc::InvalidArgument);
        }
        if (picture.gpu() || picture.format() != PixelFormat::NV12) {
            // In GPU memory, or in the other 4:2:0 layout.
            Result<VideoFrame> converted = convert_frame(picture, PixelFormat::NV12);
            if (!converted) return fail(converted.error());
            return encode(*converted, keyframe, sink);
        }
        if (!session_) {  // lost with the one before: another, starting with a keyframe
            if (Result<void> opened = open(); !opened) return opened;
            keyframe_owed_ = true;
        }

        Result<CfPtr<CVPixelBufferRef>> buffer = copy_in(picture);
        if (!buffer) return fail(buffer.error());

        // Sessions insist on increasing times; two pictures stamped alike are a
        // microsecond apart.
        int64_t pts = picture.capture_time().time_since_epoch().count();
        if (last_pts_ && pts <= *last_pts_) pts = *last_pts_ + 1;
        last_pts_ = pts;
        const CMTime time = CMTimeMake(pts, kMicroseconds);
        const CMTime duration =
            CMTimeMake(int64_t{kMicroseconds} * config_.frame_rate_den / config_.frame_rate_num,
                       kMicroseconds);

        CfPtr<CFMutableDictionaryRef> options;
        if (std::exchange(keyframe_owed_, false) || keyframe) {
            options = dictionary();
            CFDictionarySetValue(options.get(), kVTEncodeFrameOptionKey_ForceKeyFrame,
                                 kCFBooleanTrue);
        }
        OSStatus status = VTCompressionSessionEncodeFrame(
            session_.get(), buffer->get(), time, duration, options.get(), nullptr, nullptr);
        // The frame of this picture, and of none after it: out of this call.
        if (status == noErr) status = VTCompressionSessionCompleteFrames(session_.get(), time);
        return hand_out(status, sink);
    }

    void set_bitrate(uint32_t bits_per_second) override {
        if (bits_per_second == 0 || bits_per_second == config_.bitrate) return;
        config_.bitrate = bits_per_second;
        if (session_ && !apply_bitrate()) {
            LOG_WARN("engine",
                     info_.name << " kept its bitrate: " << bits_per_second << " bit/s refused");
        }
    }

    // Nothing is held back between calls; the next picture is a keyframe all
    // the same, as the interface promises.
    Result<void> flush(Sink sink) override {
        keyframe_owed_ = true;
        if (!session_) return {};
        return hand_out(VTCompressionSessionCompleteFrames(session_.get(), kCMTimeInvalid), sink);
    }

private:
    Result<void> create(bool low_latency) {
        CfPtr<CFMutableDictionaryRef> specification = dictionary();
        CFDictionarySetValue(specification.get(),
                             kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder,
                             kCFBooleanTrue);
        if (low_latency) {
            CFDictionarySetValue(specification.get(),
                                 kVTVideoEncoderSpecification_EnableLowLatencyRateControl,
                                 kCFBooleanTrue);
        }

        // Pictures as the pool hands them out: NV12 of the range the stream
        // signals, on IOSurfaces, which the GPU reads where they are.
        CfPtr<CFMutableDictionaryRef> attributes = dictionary();
        const CfPtr<CFNumberRef> format = number(int64_t{pixel_format()});
        const CfPtr<CFNumberRef> width = number(int64_t{config_.width});
        const CfPtr<CFNumberRef> height = number(int64_t{config_.height});
        const CfPtr<CFMutableDictionaryRef> surface = dictionary();
        CFDictionarySetValue(attributes.get(), kCVPixelBufferPixelFormatTypeKey, format.get());
        CFDictionarySetValue(attributes.get(), kCVPixelBufferWidthKey, width.get());
        CFDictionarySetValue(attributes.get(), kCVPixelBufferHeightKey, height.get());
        CFDictionarySetValue(attributes.get(), kCVPixelBufferIOSurfacePropertiesKey, surface.get());

        VTCompressionSessionRef session = nullptr;
        const OSStatus status = VTCompressionSessionCreate(
            kCFAllocatorDefault, static_cast<int32_t>(config_.width),
            static_cast<int32_t>(config_.height),
            config_.codec == Codec::H265 ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264,
            specification.get(), attributes.get(), kCFAllocatorDefault,
            &VideoToolboxEncoder::on_output, this, &session);
        if (status != noErr) {
            LOG_DEBUG("engine", info_.name << (low_latency ? " with low-latency rate control" : "")
                                           << " cannot open: " << status);
            return fail(Errc::Unavailable);
        }
        session_.reset(session);
        if (Result<void> tuned = tune(low_latency); !tuned) {
            close();
            return tuned;
        }
        LOG_DEBUG("engine",
                  info_.name << (low_latency ? ": low-latency rate control" : ": real time"));
        return {};
    }

    Result<void> tune(bool low_latency) {
        // What every frame must be, or the encoder is no use here. Low-latency
        // rate control never reorders, and may refuse to be told.
        const bool in_order =
            set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse) || low_latency;
        if (!in_order || !apply_bitrate()) {
            return fail(Errc::Unavailable);
        }
        // The rest as far as this encoder goes.
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
        set(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kCFBooleanTrue);
        set(kVTCompressionPropertyKey_MaxFrameDelayCount, number(int64_t{0}).get());
        set(kVTCompressionPropertyKey_ProfileLevel,
            config_.codec == Codec::H265 ? kVTProfileLevel_HEVC_Main_AutoLevel
                                         : kVTProfileLevel_H264_ConstrainedHigh_AutoLevel);
        // HEVC's keyframes IDR, each decodable on its own.
        if (config_.codec == Codec::H265)
            set(kVTCompressionPropertyKey_AllowOpenGOP, kCFBooleanFalse);
        set(kVTCompressionPropertyKey_ExpectedFrameRate,
            number(double(config_.frame_rate_num) / config_.frame_rate_den).get());
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval,
            number(config_.keyframe_interval > 0 ? int64_t{config_.keyframe_interval}
                                                 : kNoPeriodicKeyframes)
                .get());
        set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2);
        set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2);
        set(kVTCompressionPropertyKey_YCbCrMatrix, matrix());

        if (const OSStatus status = VTCompressionSessionPrepareToEncodeFrames(session_.get());
            status != noErr) {
            LOG_DEBUG("engine", info_.name << " cannot prepare: " << status);
            return fail(Errc::Unavailable);
        }
        return {};
    }

    /// Aim at the config's bitrate, a second's worth at most in any second
    /// (as FFmpeg's encoders are capped at it).
    bool apply_bitrate() {
        const int64_t bitrate = config_.bitrate;
        if (!set(kVTCompressionPropertyKey_AverageBitRate, number(bitrate).get())) return false;
        const CfPtr<CFNumberRef> bytes = number(bitrate / 8);
        const CfPtr<CFNumberRef> seconds = number(1.0);
        const void* limits[] = {bytes.get(), seconds.get()};
        const CfPtr<CFArrayRef> array(
            CFArrayCreate(kCFAllocatorDefault, limits, 2, &kCFTypeArrayCallBacks));
        set(kVTCompressionPropertyKey_DataRateLimits, array.get());
        return true;
    }

    /// Set a property of the session; false, and said so, if refused.
    bool set(CFStringRef key, CFTypeRef value) {
        const OSStatus status = VTSessionSetProperty(session_.get(), key, value);
        if (status != noErr) {
            const char* name = CFStringGetCStringPtr(key, kCFStringEncodingUTF8);
            LOG_DEBUG("engine",
                      info_.name << " refused " << (name ? name : "a property") << ": " << status);
        }
        return status == noErr;
    }

    OSType pixel_format() const {
        return config_.color_space.range == ColorRange::Full
                   ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                   : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    }

    CFStringRef matrix() const {
        return config_.color_space.matrix == ColorMatrix::BT601
                   ? kCVImageBufferYCbCrMatrix_ITU_R_601_4
                   : kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    }

    /// `picture` (NV12, in memory) in a buffer of the session's pool.
    Result<CfPtr<CVPixelBufferRef>> copy_in(const VideoFrame& picture) {
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(session_.get());
        CVPixelBufferRef created = nullptr;
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &created) !=
                         kCVReturnSuccess) {
            return fail(Errc::Internal);
        }
        CfPtr<CVPixelBufferRef> buffer(created);
        if (CVPixelBufferLockBaseAddress(created, 0) != kCVReturnSuccess) {
            return fail(Errc::Internal);
        }
        for (size_t i = 0; i < plane_count(PixelFormat::NV12); ++i) {
            auto* to = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(created, i));
            const size_t to_stride = CVPixelBufferGetBytesPerRowOfPlane(created, i);
            const VideoFrame::Plane& from = picture.plane(i);
            const size_t bytes = plane_row_bytes(PixelFormat::NV12, i, config_.width);
            for (size_t y = 0; y < plane_rows(PixelFormat::NV12, i, config_.height); ++y)
                std::memcpy(to + y * to_stride, from.data + y * from.stride, bytes);
        }
        CVPixelBufferUnlockBaseAddress(created, 0);

        CVBufferSetAttachment(created, kCVImageBufferColorPrimariesKey,
                              kCVImageBufferColorPrimaries_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(created, kCVImageBufferTransferFunctionKey,
                              kCVImageBufferTransferFunction_ITU_R_709_2,
                              kCVAttachmentMode_ShouldPropagate);
        CVBufferSetAttachment(created, kCVImageBufferYCbCrMatrixKey, matrix(),
                              kCVAttachmentMode_ShouldPropagate);
        return buffer;
    }

    /// Hand `sink` the frames the session completed, unless it failed.
    Result<void> hand_out(OSStatus status, Sink sink) {
        std::vector<CfPtr<CMSampleBufferRef>> done;
        {
            std::lock_guard lock(mutex_);
            done.swap(done_);
            const OSStatus failed = std::exchange(failed_, noErr);
            if (status == noErr) status = failed;
        }
        if (status != noErr) {
            LOG_WARN("engine", info_.name << " failed: " << status);
            if (status == kVTInvalidSessionErr) close();  // the next picture opens another
            return fail(Errc::Internal);
        }
        for (const CfPtr<CMSampleBufferRef>& sample : done) {
            Result<EncodedVideo> frame = frame_of(sample.get());
            if (!frame) return fail(frame.error());
            sink(std::move(*frame));
        }
        return {};
    }

    /// One frame of the session's, in Annex B: each NAL unit after a start
    /// code, a keyframe's parameter sets ahead of the rest.
    Result<EncodedVideo> frame_of(CMSampleBufferRef sample) {
        CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
        CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sample);
        if (!format || !data) return fail(Errc::Internal);

        size_t sets = 0;
        int length_size = 0;
        if (parameter_set(format, 0, nullptr, nullptr, &sets, &length_size) != noErr ||
            length_size < 1 || length_size > 4) {
            return fail(Errc::Internal);
        }
        CMBlockBufferRef joined = nullptr;
        if (CMBlockBufferCreateContiguous(kCFAllocatorDefault, data, kCFAllocatorDefault, nullptr,
                                          0, 0, 0, &joined) != kCMBlockBufferNoErr) {
            return fail(Errc::Internal);
        }
        const CfPtr<CMBlockBufferRef> contiguous(joined);
        size_t size = 0;
        char* bytes = nullptr;
        if (CMBlockBufferGetDataPointer(joined, 0, nullptr, &size, &bytes) != kCMBlockBufferNoErr) {
            return fail(Errc::Internal);
        }
        const std::span<const uint8_t> units(reinterpret_cast<const uint8_t*>(bytes), size);

        EncodedVideo out;
        out.info.keyframe = is_keyframe(sample);
        std::vector<std::span<const uint8_t>> parameter_sets;
        if (out.info.keyframe) {
            for (size_t i = 0; i < sets; ++i) {
                const uint8_t* set = nullptr;
                size_t set_size = 0;
                if (parameter_set(format, i, &set, &set_size, nullptr, nullptr) != noErr) {
                    return fail(Errc::Internal);
                }
                parameter_sets.emplace_back(set, set_size);
            }
        }
        size_t total = 0;
        for (std::span<const uint8_t> set : parameter_sets) total += sizeof kStartCode + set.size();
        const auto count = [&](std::span<const uint8_t> unit) {
            total += sizeof kStartCode + unit.size();
        };
        if (!for_each_unit(units, size_t(length_size), count)) return fail(Errc::Internal);

        out.payload = allocate_frame_payload(total);
        const auto append = [&](std::span<const uint8_t> unit) {
            out.payload.append(kStartCode);
            out.payload.append(unit);
        };
        for (std::span<const uint8_t> set : parameter_sets) append(set);
        for_each_unit(units, size_t(length_size), append);

        out.info.number = next_number_++;
        out.info.capture_time = Timestamp{
            Duration{CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample),
                                        kMicroseconds, kCMTimeRoundingMethod_RoundHalfAwayFromZero)
                         .value}};
        // One chain from keyframe to keyframe: each frame needs the one
        // before (and through it everything back to the keyframe).
        if (!out.info.keyframe && out.info.number > 0) out.info.references = {out.info.number - 1};
        return out;
    }

    OSStatus parameter_set(CMFormatDescriptionRef format, size_t index, const uint8_t** set,
                           size_t* size, size_t* count, int* length_size) const {
        return config_.codec == Codec::H265 ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
                                                  format, index, set, size, count, length_size)
                                            : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                                                  format, index, set, size, count, length_size);
    }

    /// On the session's thread, or inside one of its calls on ours.
    static void on_output(void* self, void*, OSStatus status, VTEncodeInfoFlags flags,
                          CMSampleBufferRef sample) {
        auto& encoder = *static_cast<VideoToolboxEncoder*>(self);
        std::lock_guard lock(encoder.mutex_);
        if (status != noErr) {
            if (encoder.failed_ == noErr) encoder.failed_ = status;
            return;
        }
        if (!sample || (flags & kVTEncodeInfo_FrameDropped)) return;
        CFRetain(sample);
        encoder.done_.emplace_back(sample);
    }

    /// Let the session go, and whatever it still had.
    void close() {
        if (!session_) return;
        VTCompressionSessionCompleteFrames(session_.get(), kCMTimeInvalid);
        VTCompressionSessionInvalidate(session_.get());
        session_.reset();
        std::lock_guard lock(mutex_);
        done_.clear();
        failed_ = noErr;
    }

    VideoEncoderConfig config_;
    const VideoEncoderInfo info_;
    CfPtr<VTCompressionSessionRef> session_;
    /// The next picture is a keyframe whether asked for or not.
    bool keyframe_owed_ = false;
    std::optional<int64_t> last_pts_;
    FrameNumber next_number_ = 0;

    std::mutex mutex_;  ///< over what the session's callback hands back
    std::vector<CfPtr<CMSampleBufferRef>> done_;
    OSStatus failed_ = noErr;
};

}  // namespace

Result<std::unique_ptr<VideoEncoder>> open_videotoolbox_encoder(const VideoEncoderConfig& config) {
    if (config.codec != Codec::H264 && config.codec != Codec::H265) {
        return fail(Errc::Unavailable);
    }
    auto encoder = std::make_unique<VideoToolboxEncoder>(config);
    if (Result<void> opened = encoder->open(); !opened) return fail(opened.error());
    return std::unique_ptr<VideoEncoder>(std::move(encoder));
}

}  // namespace ratcast::macos
