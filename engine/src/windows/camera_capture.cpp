// Cameras on Windows: Media Foundation's video capture devices, each read by
// an asynchronous source reader whose callback hands the pictures out.

#include "capture/camera_backend.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/frame_pacer.h"
#include "ratcast/engine/video_convert.h"
#include "windows/com.h"
#include "windows/media_foundation_camera.h"
#include "windows/qpc.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace mf {
namespace {

using win::ComPtr;

/// A camera mode at fewer pictures a second than this is taken only if there
/// is no other.
constexpr double kSmoothRate = 15;

std::string utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring string_attribute(IMFAttributes& attributes, const GUID& key) {
    UINT32 length = 0;
    if (FAILED(attributes.GetStringLength(key, &length))) return {};
    std::wstring value(size_t(length) + 1, L'\0');
    if (FAILED(attributes.GetString(key, value.data(), length + 1, &length))) return {};
    value.resize(length);
    return value;
}

/// Media Foundation, from the first use on this thread to the last.
class Startup {
public:
    Startup() noexcept : result_(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET)) {}
    ~Startup() {
        if (SUCCEEDED(result_)) MFShutdown();
    }
    Startup(const Startup&) = delete;
    Startup& operator=(const Startup&) = delete;
    bool ok() const noexcept { return SUCCEEDED(result_); }

private:
    HRESULT result_;
};

struct Device {
    uint32_t node = 0;
    std::string name;
    ComPtr<IMFActivate> activate;
};

/// The video capture devices there are now, in the system's order.
Result<std::vector<Device>> devices() {
    ComPtr<IMFAttributes> query;
    HRESULT result = MFCreateAttributes(&query, 1);
    if (SUCCEEDED(result))
        result = query->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(result)) result = MFEnumDeviceSources(query.Get(), &found, &count);
    if (FAILED(result)) return fail(win::errc_of(result));
    std::vector<Device> devices;
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFActivate> activate;
        activate.Attach(found[i]);
        const std::wstring link = string_attribute(
            *activate.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        std::string name =
            utf8(string_attribute(*activate.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME));
        devices.push_back(
            {node_of(link), name.empty() ? std::string("Camera") : std::move(name), activate});
    }
    CoTaskMemFree(found);
    return devices;
}

std::optional<PixelFormat> pixel_format(const GUID& subtype) {
    if (subtype == MFVideoFormat_YUY2) return PixelFormat::YUY2;
    if (subtype == MFVideoFormat_NV12) return PixelFormat::NV12;
    if (subtype == MFVideoFormat_I420 || subtype == MFVideoFormat_IYUV) return PixelFormat::I420;
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) return PixelFormat::BGRA;
    return std::nullopt;
}

/// What `type` says of a mode; nullopt for one in a format not read here.
std::optional<CameraMode> mode_of(IMFMediaType& type, uint32_t index) {
    GUID subtype{};
    if (FAILED(type.GetGUID(MF_MT_SUBTYPE, &subtype))) return std::nullopt;
    const std::optional<PixelFormat> format = pixel_format(subtype);
    if (!format) return std::nullopt;
    CameraMode mode;
    mode.format = *format;
    mode.index = index;
    if (FAILED(MFGetAttributeSize(&type, MF_MT_FRAME_SIZE, &mode.width, &mode.height)))
        return std::nullopt;
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(&type, MF_MT_FRAME_RATE, &numerator, &denominator)) &&
        denominator > 0) {
        mode.frame_rate = double(numerator) / denominator;
    }
    mode.stride = int32_t(MFGetAttributeUINT32(&type, MF_MT_DEFAULT_STRIDE, 0));
    if (MFGetAttributeUINT32(&type, MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601) ==
        MFVideoTransferMatrix_BT709) {
        mode.color_space.matrix = ColorMatrix::BT709;
    }
    if (MFGetAttributeUINT32(&type, MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) ==
        MFNominalRange_0_255) {
        mode.color_space.range = ColorRange::Full;
    }
    return mode;
}

/// A buffer's bytes, locked while this lives: the first row's start and the
/// step to the next, which is negative for a picture stored bottom row first.
class Locked {
public:
    Locked(IMFMediaBuffer& buffer, const CameraMode& mode) : buffer_(buffer) {
        // A 2D buffer says its own layout; any other is laid out as the mode says.
        if (SUCCEEDED(buffer.QueryInterface(IID_PPV_ARGS(&buffer2d_)))) {
            LONG pitch = 0;
            DWORD length = 0;
            if (SUCCEEDED(buffer2d_->Lock2D(&first_, &pitch)) &&
                SUCCEEDED(buffer2d_->GetContiguousLength(&length))) {
                pitch_ = pitch;
                length_ = length;
                begin_ = pitch < 0 ? first_ + int64_t(pitch) * (int64_t(mode.height) - 1) : first_;
                return;
            }
            if (first_) buffer2d_->Unlock2D();
            buffer2d_.Reset();
            first_ = nullptr;
        }
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer.Lock(&data, nullptr, &length))) return;
        locked_ = true;
        begin_ = data;
        length_ = length;
        pitch_ =
            mode.stride != 0 ? mode.stride : int64_t(plane_row_bytes(mode.format, 0, mode.width));
        first_ = pitch_ < 0 ? data + (-pitch_) * (int64_t(mode.height) - 1) : data;
    }
    ~Locked() {
        if (buffer2d_ && first_) buffer2d_->Unlock2D();
        if (locked_) buffer_.Unlock();
    }
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

    bool ok() const noexcept { return first_ != nullptr && pitch_ != 0; }
    const BYTE* first() const noexcept { return first_; }
    int64_t pitch() const noexcept { return pitch_; }
    /// Whether `size` bytes from `at` are within the buffer.
    bool holds(const BYTE* at, size_t size) const noexcept {
        return at >= begin_ && at + size <= begin_ + length_;
    }

private:
    IMFMediaBuffer& buffer_;
    ComPtr<IMF2DBuffer> buffer2d_;
    bool locked_ = false;
    BYTE* first_ = nullptr;
    const BYTE* begin_ = nullptr;
    size_t length_ = 0;
    int64_t pitch_ = 0;
};

}  // namespace

uint32_t node_of(std::wstring_view link) {
    uint32_t hash = 2166136261u;  // FNV-1a, over the link as Windows spells it, case aside
    for (wchar_t c : link) {
        hash = (hash ^ uint32_t(towlower(c))) * 16777619u;
    }
    return hash != 0 ? hash : 1;
}

std::optional<size_t> choose_mode(std::span<const CameraMode> modes, uint32_t max_width,
                                  uint32_t max_height) {
    const auto fits = [&](const CameraMode& mode) {
        return mode.width <= max_width && mode.height <= max_height;
    };
    for (size_t i = 0; i < modes.size(); ++i) {
        if (fits(modes[i]) && modes[i].frame_rate >= kSmoothRate) return i;
    }
    for (size_t i = 0; i < modes.size(); ++i) {
        if (fits(modes[i])) return i;
    }
    if (!modes.empty()) return 0;
    return std::nullopt;
}

std::optional<VideoFrame> read_picture(IMFMediaBuffer& buffer, const CameraMode& mode,
                                       PixelFormat format, Timestamp captured) {
    const uint32_t width = mode.width & ~1u;  // 4:2:0 out wants even sizes
    const uint32_t height = mode.height & ~1u;
    if (width == 0 || height == 0) return std::nullopt;
    const Locked locked(buffer, mode);
    if (!locked.ok()) return std::nullopt;

    if (locked.pitch() < 0) {  // RGB, bottom row first: turned the right way up
        if (mode.format != PixelFormat::BGRA) return std::nullopt;
        const size_t row = plane_row_bytes(PixelFormat::BGRA, 0, width);
        MutableVideoFrame upright = MutableVideoFrame::allocate(PixelFormat::BGRA, width, height);
        for (uint32_t y = 0; y < height; ++y) {
            const BYTE* from = locked.first() + locked.pitch() * int64_t(y);
            if (!locked.holds(from, row)) return std::nullopt;
            std::memcpy(upright.plane(0).data + upright.plane(0).stride * y, from, row);
        }
        Result<VideoFrame> picture = convert_frame(std::move(upright).freeze(captured), format);
        return picture ? std::optional<VideoFrame>(std::move(*picture)) : std::nullopt;
    }

    // The planes one after another: rows a stride apart, the luma's (halved
    // for I420's chroma), each plane as many rows as the mode's height says.
    const size_t pitch = size_t(locked.pitch());
    const size_t planes = plane_count(mode.format);
    std::array<VideoFrame::Plane, 3> view{};
    const BYTE* at = locked.first();
    for (size_t i = 0; i < planes; ++i) {
        const size_t stride = i > 0 && mode.format == PixelFormat::I420 ? pitch / 2 : pitch;
        const size_t rows = plane_rows(mode.format, i, mode.height);
        if (!locked.holds(at, (rows - 1) * stride + plane_row_bytes(mode.format, i, mode.width)))
            return std::nullopt;
        view[i] = {at, stride};
        at += stride * rows;
    }
    const VideoFrame picture(mode.format, width, height, {view.data(), planes}, nullptr, captured,
                             mode.color_space);
    Result<VideoFrame> converted = convert_frame(picture, format);
    return converted ? std::optional<VideoFrame>(std::move(*converted)) : std::nullopt;
}

namespace {

/// One camera, read from start() to stop(): the source reader's callback, and
/// everything a picture needs.
class Reader final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback> {
public:
    Reader(const CameraOptions& options, Clock& clock, VideoSource::Sink sink,
           VideoSource::EndHandler ended)
        : options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          pacer_(options.max_frame_rate),
          flushed_(win::make_event(true)) {}

    /// Open `device` in the mode its modes call for, and ask for the first
    /// picture.
    Result<void> open(IMFActivate& device) {
        if (!flushed_) return fail(Errc::Internal);
        HRESULT result = device.ActivateObject(IID_PPV_ARGS(&source_));
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(result)) result = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(result))
            result = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, this);
        if (SUCCEEDED(result))
            result = MFCreateSourceReaderFromMediaSource(source_.Get(), attributes.Get(), &reader_);
        if (FAILED(result)) {
            LOG_WARN("engine", "cannot open the camera: HRESULT " << std::hex << result);
            return fail(win::errc_of(result));
        }

        std::vector<CameraMode> modes;
        ComPtr<IMFMediaType> type;
        for (DWORD i = 0; SUCCEEDED(reader_->GetNativeMediaType(kStream, i, &type)); ++i) {
            if (std::optional<CameraMode> mode = mode_of(*type.Get(), i)) modes.push_back(*mode);
            type.Reset();
        }
        const std::optional<size_t> chosen =
            choose_mode(modes, options_.max_width, options_.max_height);
        if (!chosen) {
            LOG_WARN("engine", "the camera sends pictures in no format read here");
            return fail(Errc::Unsupported);
        }
        mode_ = modes[*chosen];
        result = reader_->GetNativeMediaType(kStream, mode_.index, &type);
        if (SUCCEEDED(result)) result = reader_->SetCurrentMediaType(kStream, nullptr, type.Get());
        if (SUCCEEDED(result)) result = reader_->SetStreamSelection(kStream, TRUE);
        if (FAILED(result)) return fail(win::errc_of(result));
        LOG_INFO("engine", "camera: " << mode_.width << "x" << mode_.height << " at "
                                      << mode_.frame_rate << " a second");

        std::lock_guard lock(mutex_);
        running_ = true;
        result = reader_->ReadSample(kStream, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(result)) {
            running_ = false;
            return fail(win::errc_of(result));
        }
        return {};
    }

    /// No picture handed out from here on; the reader flushed and let go of.
    void close() {
        {
            std::lock_guard lock(mutex_);
            running_ = false;
        }
        if (reader_ && SUCCEEDED(reader_->Flush(kStream)))
            WaitForSingleObject(flushed_.get(), 5000);
        reader_.Reset();
        if (source_) source_->Shutdown();
        source_.Reset();
    }

    STDMETHOD(OnReadSample)(HRESULT status, DWORD, DWORD flags, LONGLONG,
                            IMFSample* sample) override {
        std::lock_guard lock(mutex_);
        if (!running_) return S_OK;
        if (status == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED ||
            (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            end(Result<void>{});  // unplugged, or taken away
            return S_OK;
        }
        if (FAILED(status) || (flags & MF_SOURCE_READERF_ERROR)) {
            LOG_WARN("engine", "the camera failed: HRESULT " << std::hex << status);
            end(fail(Errc::Unavailable));
            return S_OK;
        }
        if (sample && pacer_.admit(clock_.now())) {
            if (std::optional<VideoFrame> picture = read(*sample)) sink_(std::move(*picture));
        }
        if (FAILED(reader_->ReadSample(kStream, 0, nullptr, nullptr, nullptr, nullptr)))
            end(fail(Errc::Unavailable));
        return S_OK;
    }

    STDMETHOD(OnFlush)(DWORD) override {
        SetEvent(flushed_.get());
        return S_OK;
    }

    STDMETHOD(OnEvent)(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    static constexpr DWORD kStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    std::optional<VideoFrame> read(IMFSample& sample) {
        // Stamped by the device's clock, the performance counter, if it says.
        Timestamp captured = clock_.now();
        UINT64 stamp = 0;
        if (SUCCEEDED(sample.GetUINT64(MFSampleExtension_DeviceTimestamp, &stamp)) && stamp != 0)
            captured = std::min(win::at_qpc(int64_t(stamp), clock_), captured);
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample.ConvertToContiguousBuffer(&buffer))) return std::nullopt;
        return read_picture(*buffer.Get(), mode_, options_.format, captured);
    }

    void end(Result<void> outcome) {
        running_ = false;
        if (ended_) ended_(outcome);
    }

    const CameraOptions options_;
    Clock& clock_;
    const VideoSource::Sink sink_;
    const VideoSource::EndHandler ended_;

    std::mutex mutex_;  ///< running_, and a picture being handed out
    bool running_ = false;
    FramePacer pacer_;
    CameraMode mode_;
    win::Handle flushed_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
};

class Camera final : public VideoSource {
public:
    Camera(const CameraOptions& options, Clock& clock = SteadyClock::instance())
        : options_(options), clock_(clock) {}
    ~Camera() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        std::lock_guard lock(mutex_);
        if (reader_ || missing_.joinable()) return fail(Errc::InvalidArgument);
        win::keep_multithreaded_apartment();
        if (!startup_.ok()) return fail(Errc::Unavailable);
        Result<std::vector<Device>> found = devices();
        if (!found) return fail(found.error());
        const auto device = std::find_if(found->begin(), found->end(), [this](const Device& d) {
            return d.node == options_.node;
        });
        if (device == found->end()) {
            // Not there: the capture ends as it starts, as a capture whose
            // camera went does.
            LOG_WARN("engine", "camera " << options_.node << " is not there");
            missing_ = std::thread([ended = std::move(ended)] {
                if (ended) ended(fail(Errc::Unavailable));
            });
            return {};
        }
        auto reader =
            Microsoft::WRL::Make<Reader>(options_, clock_, std::move(sink), std::move(ended));
        if (!reader) return fail(Errc::Internal);
        if (Result<void> opened = reader->open(*device->activate.Get()); !opened) {
            reader->close();
            device->activate->ShutdownObject();
            return opened;
        }
        reader_ = std::move(reader);
        device_ = device->activate;
        return {};
    }

    void stop() override {
        std::lock_guard lock(mutex_);
        if (missing_.joinable()) missing_.join();
        if (!reader_) return;
        reader_->close();
        reader_.Reset();
        device_->ShutdownObject();
        device_.Reset();
    }

private:
    const CameraOptions options_;
    Clock& clock_;
    const Startup startup_;
    std::mutex mutex_;  ///< serialises start() and stop()
    ComPtr<Reader> reader_;
    ComPtr<IMFActivate> device_;
    std::thread missing_;  ///< saying a camera that is not there ended
};

}  // namespace
}  // namespace mf

namespace backend {

Result<std::vector<CameraInfo>> list_cameras() {
    const win::ComApartment com;
    const mf::Startup startup;
    if (!startup.ok()) return fail(Errc::Unavailable);
    Result<std::vector<mf::Device>> found = mf::devices();
    if (!found) return fail(found.error());
    std::vector<CameraInfo> cameras;
    for (const mf::Device& device : *found) cameras.push_back({device.node, device.name});
    return cameras;
}

Result<std::unique_ptr<VideoSource>> open_camera(const CameraOptions& options) {
    CameraOptions resolved = options;
    if (resolved.node == 0) {
        Result<std::vector<CameraInfo>> cameras = list_cameras();
        if (!cameras) return fail(cameras.error());
        if (cameras->empty()) {
            LOG_INFO("engine", "no camera here");
            return fail(Errc::Unavailable);
        }
        resolved.node = cameras->front().node;
    }
    return std::unique_ptr<VideoSource>(std::make_unique<mf::Camera>(resolved));
}

}  // namespace backend
}  // namespace ratcast
