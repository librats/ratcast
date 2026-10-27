// Core Audio's HAL for capture, an output unit for playback. Built with ARC:
// the queues, the listener blocks and the tap's description are released by
// it; Core Foundation's objects by hand.

#include "macos/core_audio.h"

#import <Foundation/Foundation.h>

#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/CoreAudio.h>

#include <dispatch/dispatch.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>

#include "audio/sample_clock.h"

#include "librats/util/logger.h"

namespace ratcast::core_audio {
namespace {

using namespace std::chrono_literals;

/// What the UIDs of this process's own aggregate devices start with: private,
/// they are listed to it alone, and never as a device to choose.
constexpr char kOwnDevicePrefix[] = "ratcast-tap-";

AudioObjectPropertyAddress address(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
std::optional<T> get(AudioObjectID object, AudioObjectPropertyAddress where) {
    T value{};
    UInt32 size = sizeof value;
    if (AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) != noErr)
        return std::nullopt;
    return value;
}

std::string utf8(CFStringRef text) {
    if (!text) return {};
    if (const char* fast = CFStringGetCStringPtr(text, kCFStringEncodingUTF8)) return fast;
    const CFIndex most =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(most), '\0');
    if (!CFStringGetCString(text, out.data(), most, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

/// A string property, such as a device's UID or name.
std::string string_property(AudioObjectID object, AudioObjectPropertySelector selector) {
    std::optional<CFStringRef> text = get<CFStringRef>(object, address(selector));
    if (!text || !*text) return {};
    std::string out = utf8(*text);
    CFRelease(*text);
    return out;
}

/// The objects a list property holds: devices, streams.
std::vector<AudioObjectID> object_list(AudioObjectID object, AudioObjectPropertyAddress where) {
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &where, 0, nullptr, &size) != noErr) return {};
    std::vector<AudioObjectID> objects(size / sizeof(AudioObjectID));
    if (objects.empty() ||
        AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, objects.data()) != noErr) {
        return {};
    }
    objects.resize(size / sizeof(AudioObjectID));
    return objects;
}

/// How many channels a device has in `scope`, its streams' together.
uint32_t channel_count(AudioObjectID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress where = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr || size == 0)
        return 0;
    std::vector<std::byte> storage(size);
    auto* buffers = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, buffers) != noErr) return 0;
    uint32_t channels = 0;
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
        channels += buffers->mBuffers[i].mNumberChannels;
    return channels;
}

AudioObjectPropertyScope scope_of(bool input) {
    return input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
}

/// The device of `uid` if it is there, alive, and has channels that way.
AudioObjectID device_of(const std::string& uid, bool input) {
    CFStringRef text = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
    if (!text) return kAudioObjectUnknown;
    AudioObjectPropertyAddress where = address(kAudioHardwarePropertyTranslateUIDToDevice);
    AudioObjectID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    const OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &where,
                                                       sizeof text, &text, &size, &device);
    CFRelease(text);
    if (status != noErr || device == kAudioObjectUnknown) return kAudioObjectUnknown;
    if (get<UInt32>(device, address(kAudioDevicePropertyDeviceIsAlive)).value_or(0) == 0 ||
        channel_count(device, scope_of(input)) == 0) {
        return kAudioObjectUnknown;
    }
    return device;
}

/// The system's default input or output, kAudioObjectUnknown with none.
AudioObjectID default_device(bool input) {
    return get<AudioObjectID>(kAudioObjectSystemObject,
                              address(input ? kAudioHardwarePropertyDefaultInputDevice
                                            : kAudioHardwarePropertyDefaultOutputDevice))
        .value_or(kAudioObjectUnknown);
}

double nominal_rate(AudioObjectID device) {
    return get<Float64>(device, address(kAudioDevicePropertyNominalSampleRate)).value_or(0);
}

/// How far behind the device's stamps sound is captured, or ahead of them it
/// is heard: the device's latency and its first stream's in that direction.
Duration device_latency(AudioObjectID device, bool input) {
    const double rate = nominal_rate(device);
    if (rate <= 0) return {};
    const AudioObjectPropertyScope scope = scope_of(input);
    UInt32 frames = get<UInt32>(device, address(kAudioDevicePropertyLatency, scope)).value_or(0);
    const std::vector<AudioObjectID> streams =
        object_list(device, address(kAudioDevicePropertyStreams, scope));
    if (!streams.empty())
        frames += get<UInt32>(streams.front(), address(kAudioStreamPropertyLatency)).value_or(0);
    return Duration{static_cast<int64_t>(frames * 1e6 / rate)};
}

/// The local clock's time at a host time (mach_absolute_time's ticks).
Timestamp at_host_time(Clock& clock, uint64_t host) {
    const Timestamp now = clock.now();
    const uint64_t host_now = AudioGetCurrentHostTime();
    const int64_t ns = host >= host_now ? int64_t(AudioConvertHostTimeToNanos(host - host_now))
                                        : -int64_t(AudioConvertHostTimeToNanos(host_now - host));
    return now + Duration{ns / 1000};
}

/// Interleaved 32-bit float samples.
AudioStreamBasicDescription float_format(double rate, uint32_t channels) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = UInt32(sizeof(float) * channels);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return format;
}

/// Interleaved float samples from one rate to another, a block at a time.
class RateConverter {
public:
    RateConverter(uint32_t channels, double from, double to)
        : channels_(channels), ratio_(to / from) {
        const AudioStreamBasicDescription in = float_format(from, channels);
        const AudioStreamBasicDescription out = float_format(to, channels);
        if (AudioConverterNew(&in, &out, &converter_) != noErr) converter_ = nullptr;
    }
    RateConverter(const RateConverter&) = delete;
    RateConverter& operator=(const RateConverter&) = delete;
    ~RateConverter() {
        if (converter_) AudioConverterDispose(converter_);
    }

    bool ok() const noexcept { return converter_ != nullptr; }

    /// What the converter has to give now for `in` (whole sample times), into
    /// `out`, which it sizes: a little held back at first, then all of it.
    void convert(std::span<const float> in, std::vector<float>& out) {
        Input input{in.data(), UInt32(in.size() / channels_), channels_};
        const size_t room = size_t(std::ceil(input.frames * ratio_)) + 64;
        out.resize(room * channels_);
        AudioBufferList buffers{};
        buffers.mNumberBuffers = 1;
        buffers.mBuffers[0] = {channels_, UInt32(out.size() * sizeof(float)), out.data()};
        UInt32 frames = UInt32(room);
        const OSStatus status =
            AudioConverterFillComplexBuffer(converter_, supply, &input, &frames, &buffers, nullptr);
        if (status != noErr && status != kNoMoreInputNow) frames = 0;
        out.resize(size_t{frames} * channels_);
    }

private:
    /// What supply() says once the block is all handed over: the converter
    /// keeps what it has and gives what it can.
    static constexpr OSStatus kNoMoreInputNow = -100'001;

    struct Input {
        const float* data;
        UInt32 frames;
        uint32_t channels;
    };

    static OSStatus supply(AudioConverterRef, UInt32* frames, AudioBufferList* buffers,
                           AudioStreamPacketDescription**, void* context) {
        auto* input = static_cast<Input*>(context);
        if (input->frames == 0) {
            *frames = 0;
            return kNoMoreInputNow;
        }
        buffers->mNumberBuffers = 1;
        buffers->mBuffers[0].mNumberChannels = input->channels;
        buffers->mBuffers[0].mData = const_cast<float*>(input->data);
        buffers->mBuffers[0].mDataByteSize =
            UInt32(input->frames * input->channels * sizeof(float));
        *frames = input->frames;
        input->frames = 0;
        return noErr;
    }

    const uint32_t channels_;
    const double ratio_;
    AudioConverterRef converter_ = nullptr;
};

/// A process's audio object: a process that has used Core Audio, ours
/// included, has one. kAudioObjectUnknown for one that has not.
AudioObjectID process_object(pid_t process) {
    AudioObjectPropertyAddress where = address(kAudioHardwarePropertyTranslatePIDToProcessObject);
    AudioObjectID object = kAudioObjectUnknown;
    UInt32 size = sizeof object;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &where, sizeof process, &process,
                                   &size, &object) != noErr) {
        return kAudioObjectUnknown;
    }
    return object;
}

/// A tap and the private aggregate device it is heard through.
struct Tap {
    AudioObjectID tap = kAudioObjectUnknown;
    AudioObjectID device = kAudioObjectUnknown;
    double rate = 0;
};

void destroy(Tap& tap) {
    if (tap.device != kAudioObjectUnknown) {
        if (@available(macOS 14.2, *)) AudioHardwareDestroyAggregateDevice(tap.device);
    }
    if (tap.tap != kAudioObjectUnknown) {
        if (@available(macOS 14.2, *)) AudioHardwareDestroyProcessTap(tap.tap);
    }
    tap = {};
}

/// A tap as `description` says, and an aggregate device of it alone.
API_AVAILABLE(macos(14.2))
Result<Tap> make_tap(CATapDescription* description) {
    [description setName:@"ratcast"];
    [description setPrivate:YES];
    [description setMuteBehavior:CATapUnmuted];
    Tap tap;
    OSStatus status = AudioHardwareCreateProcessTap(description, &tap.tap);
    if (status != noErr) {
        LOG_WARN("engine", "cannot tap the computer's sound: OSStatus " << status);
        return fail(Errc::Unavailable);
    }

    NSString* uid = [[NSString stringWithUTF8String:kOwnDevicePrefix]
        stringByAppendingString:[NSUUID UUID].UUIDString];
    NSDictionary* aggregate = @{
        @kAudioAggregateDeviceNameKey : @"ratcast",
        @kAudioAggregateDeviceUIDKey : uid,
        @kAudioAggregateDeviceIsPrivateKey : @YES,
        @kAudioAggregateDeviceTapAutoStartKey : @NO,
        @kAudioAggregateDeviceTapListKey : @[ @{
            @kAudioSubTapUIDKey : description.UUID.UUIDString,
            @kAudioSubTapDriftCompensationKey : @YES,
        } ],
    };
    status = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &tap.device);
    if (status != noErr) {
        LOG_WARN("engine", "cannot hear the tapped sound: OSStatus " << status);
        destroy(tap);
        return fail(Errc::Unavailable);
    }
    tap.rate = nominal_rate(tap.device);
    return tap;
}

/// The samples of one IO cycle's buffers, each input channel where it is.
struct ChannelView {
    const float* data;
    uint32_t stride;
};

}  // namespace

// ── Capture ─────────────────────────────────────────────────────────────────

class AudioCapture::Stream {
public:
    Stream(const AudioCaptureOptions& options, Clock& clock, Sink sink, EndHandler ended)
        : options_(options),
          clock_(clock),
          sink_(std::move(sink)),
          ended_(std::move(ended)),
          sample_clock_(options.format.sample_rate),
          queue_(dispatch_queue_create("ratcast.audio-capture", DISPATCH_QUEUE_SERIAL)) {}

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    ~Stream() {
        dispatch_sync(queue_, ^{
            stopped_ = true;
            close();
            unwatch_default();
            if (process_exit_) dispatch_source_cancel(process_exit_);
        });
        // What the system queued before its listeners were removed runs, and
        // finds the stream stopped.
        dispatch_sync(queue_, ^{});
    }

    Result<void> start() {
        __block Result<void> started;
        dispatch_sync(queue_, ^{ started = begin(); });
        return started;
    }

private:
    /// What the computer plays, through a tap, rather than a microphone.
    bool tapped() const noexcept {
        return options_.node != 0 || options_.source != AudioCaptureSource::Microphone;
    }

    /// On the queue: the first open, and what the capture follows.
    Result<void> begin() {
        if (options_.node != 0) {
            const pid_t process = pid_t(options_.node);
            if (kill(process, 0) != 0 && errno == ESRCH) {
                LOG_WARN("engine", "process " << options_.node << " does not exist");
                dispatch_async(queue_, ^{ end(fail(Errc::Unavailable)); });
                return {};
            }
            process_exit_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, uintptr_t(process),
                                                   DISPATCH_PROC_EXIT, queue_);
            if (process_exit_) {
                dispatch_source_set_event_handler(process_exit_, ^{ end(Result<void>{}); });
                dispatch_resume(process_exit_);
            }
        }
        if (Result<void> opened = open(); !opened) return opened;
        // A microphone of no device chosen, or one not there, follows the default.
        if (!tapped() && !chosen_) watch_default();
        return {};
    }

    /// On the queue: the device, the IOProc on it, started.
    Result<void> open() {
        Result<void> found = tapped() ? open_tap() : open_input();
        if (!found) return found;
        if (input_rate_ <= 0) {
            close();
            return fail(Errc::Unavailable);
        }
        converter_.reset();
        if (std::abs(input_rate_ - options_.format.sample_rate) > 0.5) {
            converter_ = std::make_unique<RateConverter>(options_.format.channels, input_rate_,
                                                         options_.format.sample_rate);
            if (!converter_->ok()) {
                close();
                return fail(Errc::Internal);
            }
        }
        sample_clock_.reset();

        OSStatus status = AudioDeviceCreateIOProcIDWithBlock(
            &proc_, device_, nullptr,
            ^(const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp* input_time,
              AudioBufferList* output, const AudioTimeStamp*) {
                if (output) {
                    for (UInt32 i = 0; i < output->mNumberBuffers; ++i)
                        std::memset(output->mBuffers[i].mData, 0,
                                    output->mBuffers[i].mDataByteSize);
                }
                if (input && input_time) take(*input, *input_time);
            });
        if (status == noErr) {
            status = AudioDeviceStart(device_, proc_);
            if (status != noErr) {
                AudioDeviceDestroyIOProcID(device_, proc_);
                proc_ = nullptr;
            }
        }
        if (status != noErr) {
            LOG_WARN("engine", "cannot start the sound device: OSStatus " << status);
            close();
            return fail(Errc::Unavailable);
        }
        watch_device();
        return {};
    }

    /// On the queue: the microphone chosen, or the default.
    Result<void> open_input() {
        chosen_ = false;
        device_ = kAudioObjectUnknown;
        if (!options_.device.empty()) {
            device_ = device_of(options_.device, true);
            if (device_ != kAudioObjectUnknown) {
                chosen_ = true;
            } else {
                LOG_WARN("engine", "sound device " << options_.device
                                                   << " is not there: the default in its place");
            }
        }
        if (device_ == kAudioObjectUnknown) device_ = default_device(true);
        if (device_ == kAudioObjectUnknown) return fail(Errc::Unavailable);
        input_rate_ = nominal_rate(device_);
        input_latency_ = device_latency(device_, true);
        // Blocks as long as the latency asked, as far as the device goes.
        UInt32 frames = UInt32(std::max(1.0, input_rate_ * options_.latency.count() / 1e6));
        if (const std::optional<AudioValueRange> range =
                get<AudioValueRange>(device_, address(kAudioDevicePropertyBufferFrameSizeRange))) {
            frames = UInt32(std::clamp<double>(frames, range->mMinimum, range->mMaximum));
        }
        AudioObjectPropertyAddress where = address(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(device_, &where, 0, nullptr, sizeof frames, &frames);
        return {};
    }

    /// On the queue: a tap of what `options_` asks for, and its device.
    Result<void> open_tap() {
        if (@available(macOS 14.2, *)) {
            CATapDescription* description = nil;
            if (options_.node != 0) {
                const AudioObjectID process = process_object(pid_t(options_.node));
                if (process == kAudioObjectUnknown) {
                    LOG_WARN("engine", "process " << options_.node << " has played no sound");
                    return fail(Errc::Unavailable);
                }
                description =
                    [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(process) ]];
            } else if (options_.source == AudioCaptureSource::Applications) {
                const pid_t excluded =
                    options_.excluded_process != 0 ? pid_t(options_.excluded_process) : getpid();
                NSMutableArray<NSNumber*>* processes = [NSMutableArray array];
                if (const AudioObjectID object = process_object(excluded);
                    object != kAudioObjectUnknown) {
                    [processes addObject:@(object)];
                } else {
                    LOG_WARN("engine",
                             "process " << excluded << " has played no sound: none left out");
                }
                description =
                    [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:processes];
            } else {
                // What one output plays: its first stream's, every process's.
                description = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
                if (!options_.device.empty()) {
                    if (device_of(options_.device, false) != kAudioObjectUnknown) {
                        [description setDeviceUID:@(options_.device.c_str())];
                        [description setStream:@0];
                    } else {
                        LOG_WARN("engine", "sound device " << options_.device
                                                           << " is not there: all in its place");
                    }
                }
            }
            Result<Tap> tap = make_tap(description);
            if (!tap) return fail(tap.error());
            tap_ = *tap;
            device_ = tap_.device;
            input_rate_ = tap_.rate;
            input_latency_ = {};
            return {};
        }
        LOG_WARN("engine", "the computer's sound is heard on macOS 14.2 and later");
        return fail(Errc::Unavailable);
    }

    /// On the queue: the IOProc stopped, the device let go of.
    void close() {
        unwatch_device();
        if (proc_) {
            AudioDeviceStop(device_, proc_);
            AudioDeviceDestroyIOProcID(device_, proc_);
            proc_ = nullptr;
        }
        destroy(tap_);
        device_ = kAudioObjectUnknown;
    }

    /// On the queue: the capture's end, said once.
    void end(Result<void> outcome) {
        if (stopped_ || over_) return;
        over_ = true;
        close();
        unwatch_default();
        if (process_exit_) dispatch_source_cancel(process_exit_);
        if (ended_) ended_(std::move(outcome));
    }

    /// On the queue: the same capture on whatever the device is now.
    void reopen() {
        if (stopped_ || over_) return;
        close();
        if (Result<void> opened = open(); !opened) {
            // Following the default, this means there is none left.
            end(opened.error() == Errc::Unavailable ? Result<void>{} : opened);
        }
    }

    void watch_default() {
        default_changed_ = ^(UInt32, const AudioObjectPropertyAddress*) {
            if (default_device(true) != device_) reopen();
        };
        AudioObjectPropertyAddress where = address(kAudioHardwarePropertyDefaultInputDevice);
        AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &where, queue_,
                                            default_changed_);
    }

    void unwatch_default() {
        if (!default_changed_) return;
        AudioObjectPropertyAddress where = address(kAudioHardwarePropertyDefaultInputDevice);
        AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &where, queue_,
                                               default_changed_);
        default_changed_ = nil;
    }

    /// The device's going (one chosen), its rate changed (a tap's too).
    void watch_device() {
        device_changed_ = ^(UInt32, const AudioObjectPropertyAddress*) {
            if (stopped_ || over_) return;
            if (chosen_ &&
                get<UInt32>(device_, address(kAudioDevicePropertyDeviceIsAlive)).value_or(0) == 0) {
                end(Result<void>{});
                return;
            }
            const double rate = nominal_rate(device_);
            if (rate > 0 && rate != input_rate_) reopen();
        };
        watched_ = device_;
        for (const AudioObjectPropertyAddress& where : kWatched)
            AudioObjectAddPropertyListenerBlock(watched_, &where, queue_, device_changed_);
    }

    void unwatch_device() {
        if (!device_changed_) return;
        for (const AudioObjectPropertyAddress& where : kWatched)
            AudioObjectRemovePropertyListenerBlock(watched_, &where, queue_, device_changed_);
        device_changed_ = nil;
    }

    static constexpr AudioObjectPropertyAddress kWatched[] = {
        {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
        {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
         kAudioObjectPropertyElementMain},
    };

    /// On the device's IO thread: one cycle's input, in the format asked.
    void take(const AudioBufferList& buffers, const AudioTimeStamp& time) {
        channels_.clear();
        size_t frames = 0;
        for (UInt32 i = 0; i < buffers.mNumberBuffers; ++i) {
            const AudioBuffer& buffer = buffers.mBuffers[i];
            if (!buffer.mData || buffer.mNumberChannels == 0) continue;
            frames = buffer.mDataByteSize / (sizeof(float) * buffer.mNumberChannels);
            for (uint32_t c = 0; c < buffer.mNumberChannels; ++c)
                channels_.push_back(
                    {static_cast<const float*>(buffer.mData) + c, buffer.mNumberChannels});
        }
        if (channels_.empty() || frames == 0) return;

        const AudioFormat& format = options_.format;
        mapped_.resize(frames * format.channels);
        map_channels(frames);
        std::span<const float> samples = mapped_;
        if (converter_) {
            converter_->convert(mapped_, converted_);
            samples = converted_;
        }
        const size_t length = samples.size() / format.channels;
        if (length == 0) return;

        // Complete when its last sample was captured.
        const Duration duration = audio_duration(frames, uint32_t(input_rate_));
        const Timestamp complete =
            (time.mFlags & kAudioTimeStampHostTimeValid)
                ? at_host_time(clock_, time.mHostTime) - input_latency_ + duration
                : clock_.now();
        MutableAudioFrame block = MutableAudioFrame::allocate(format, length);
        std::copy(samples.begin(), samples.end(), block.samples().begin());
        sink_(std::move(block).freeze(sample_clock_.stamp(length, complete)));
    }

    /// The device's channels as many as asked: one is all of them mixed, one
    /// of the device's is heard on each asked, and otherwise each its own.
    void map_channels(size_t frames) {
        const size_t in = channels_.size();
        const size_t out = options_.format.channels;
        float* to = mapped_.data();
        if (out == 1) {
            for (size_t i = 0; i < frames; ++i) {
                float sum = 0;
                for (const ChannelView& channel : channels_)
                    sum += channel.data[i * channel.stride];
                to[i] = sum / float(in);
            }
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            for (size_t k = 0; k < out; ++k) {
                const size_t from = in == 1 ? 0 : k;
                to[i * out + k] =
                    from < in ? channels_[from].data[i * channels_[from].stride] : 0.0f;
            }
        }
    }

    const AudioCaptureOptions options_;
    Clock& clock_;
    const Sink sink_;
    const EndHandler ended_;
    SampleClock sample_clock_;
    dispatch_queue_t queue_;

    // The queue's, and the IO thread's while the IOProc runs (set before it
    // starts, kept until it has stopped).
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_ = nullptr;
    Tap tap_;
    bool chosen_ = false;  ///< a device chosen, which the capture keeps to
    double input_rate_ = 0;
    Duration input_latency_{};
    std::unique_ptr<RateConverter> converter_;

    // The queue's alone.
    bool stopped_ = false;
    bool over_ = false;  ///< ended by itself
    AudioObjectPropertyListenerBlock default_changed_ = nil;
    AudioObjectPropertyListenerBlock device_changed_ = nil;
    AudioObjectID watched_ = kAudioObjectUnknown;
    dispatch_source_t process_exit_ = nil;

    // The IO thread's alone.
    std::vector<ChannelView> channels_;
    std::vector<float> mapped_;
    std::vector<float> converted_;
};

AudioCapture::AudioCapture(const AudioCaptureOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioCapture::~AudioCapture() { stop(); }

Result<void> AudioCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(sink), std::move(ended));
    if (Result<void> started = stream->start(); !started) return started;
    stream_ = std::move(stream);
    return {};
}

void AudioCapture::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

// ── Playback ────────────────────────────────────────────────────────────────

class AudioPlayback::Stream {
public:
    Stream(const AudioOutputOptions& options, Clock& clock, Pull pull, EndHandler ended)
        : options_(options),
          clock_(clock),
          pull_(std::move(pull)),
          ended_(std::move(ended)),
          queue_(dispatch_queue_create("ratcast.audio-playback", DISPATCH_QUEUE_SERIAL)) {}

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    ~Stream() {
        dispatch_sync(queue_, ^{
            stopped_ = true;
            close();
        });
        dispatch_sync(queue_, ^{});
    }

    Result<void> start() {
        __block Result<void> started;
        dispatch_sync(queue_, ^{
            started = open();
            if (!started) close();
        });
        return started;
    }

private:
    /// On the queue: the unit on its device, pulling.
    Result<void> open() {
        if (options_.node != 0) return fail(Errc::Unavailable);
        AudioObjectID chosen = kAudioObjectUnknown;
        if (!options_.device.empty()) {
            chosen = device_of(options_.device, false);
            if (chosen == kAudioObjectUnknown)
                LOG_WARN("engine", "sound device " << options_.device
                                                   << " is not there: the default in its place");
        }
        // The default output's own unit follows the default as it changes.
        const AudioComponentDescription description{kAudioUnitType_Output,
                                                    chosen != kAudioObjectUnknown
                                                        ? kAudioUnitSubType_HALOutput
                                                        : kAudioUnitSubType_DefaultOutput,
                                                    kAudioUnitManufacturer_Apple, 0, 0};
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) {
            unit_ = nullptr;
            return fail(Errc::Unavailable);
        }
        OSStatus status = noErr;
        if (chosen != kAudioObjectUnknown) {
            status = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                          kAudioUnitScope_Global, 0, &chosen, sizeof chosen);
        }
        const AudioStreamBasicDescription format =
            float_format(options_.format.sample_rate, options_.format.channels);
        if (status == noErr) {
            status = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat,
                                          kAudioUnitScope_Input, 0, &format, sizeof format);
        }
        const AURenderCallbackStruct callback{render, this};
        if (status == noErr) {
            status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                          kAudioUnitScope_Input, 0, &callback, sizeof callback);
        }
        if (status == noErr) status = AudioUnitInitialize(unit_);
        if (status != noErr) {
            LOG_WARN("engine", "cannot set the sound device up: OSStatus " << status);
            return fail(Errc::Unavailable);
        }
        initialized_ = true;
        device_ = current_device();
        if (device_ == kAudioObjectUnknown) return fail(Errc::Unavailable);
        set_buffer();
        update_latency();
        watch(chosen != kAudioObjectUnknown);
        status = AudioOutputUnitStart(unit_);
        if (status != noErr) {
            LOG_WARN("engine", "cannot start the sound device: OSStatus " << status);
            return fail(Errc::Unavailable);
        }
        started_ = true;
        return {};
    }

    /// On the queue: the unit stopped and let go of.
    void close() {
        unwatch();
        if (!unit_) return;
        if (started_) AudioOutputUnitStop(unit_);
        if (initialized_) AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
        started_ = initialized_ = false;
    }

    void end(Result<void> outcome) {
        if (stopped_ || over_) return;
        over_ = true;
        close();
        if (ended_) ended_(std::move(outcome));
    }

    AudioObjectID current_device() const {
        AudioObjectID device = kAudioObjectUnknown;
        UInt32 size = sizeof device;
        if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                 kAudioUnitScope_Global, 0, &device, &size) != noErr) {
            return kAudioObjectUnknown;
        }
        return device;
    }

    /// Asked for sound a latency's length at a time, as far as the device goes.
    void set_buffer() {
        UInt32 frames =
            UInt32(std::max(1.0, nominal_rate(device_) * options_.latency.count() / 1e6));
        if (const std::optional<AudioValueRange> range =
                get<AudioValueRange>(device_, address(kAudioDevicePropertyBufferFrameSizeRange))) {
            frames = UInt32(std::clamp<double>(frames, range->mMinimum, range->mMaximum));
        }
        AudioUnitSetProperty(unit_, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0,
                             &frames, sizeof frames);
    }

    /// How long after the unit's stamps sound is heard: the device's latency
    /// and the unit's own (its conversion).
    void update_latency() {
        Float64 unit_seconds = 0;
        UInt32 size = sizeof unit_seconds;
        if (AudioUnitGetProperty(unit_, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
                                 &unit_seconds, &size) != noErr) {
            unit_seconds = 0;
        }
        const Duration latency =
            device_latency(device_, false) + Duration{int64_t(unit_seconds * 1e6)};
        latency_us_.store(latency.count(), std::memory_order_relaxed);
    }

    /// A device chosen: its going ends the playback. The default: its latency
    /// followed as it changes, the unit following it by itself.
    void watch(bool chosen) {
        listener_ = ^(UInt32, const AudioObjectPropertyAddress*) {
            if (stopped_ || over_) return;
            if (watched_ == kAudioObjectSystemObject) {
                const AudioObjectID device = default_device(false);
                if (device == kAudioObjectUnknown) return;
                device_ = device;
                update_latency();
            } else if (get<UInt32>(device_, address(kAudioDevicePropertyDeviceIsAlive))
                           .value_or(0) == 0) {
                end(Result<void>{});
            }
        };
        watched_ = chosen ? device_ : kAudioObjectSystemObject;
        watched_address_ = chosen ? address(kAudioDevicePropertyDeviceIsAlive)
                                  : address(kAudioHardwarePropertyDefaultOutputDevice);
        AudioObjectAddPropertyListenerBlock(watched_, &watched_address_, queue_, listener_);
    }

    void unwatch() {
        if (!listener_) return;
        AudioObjectRemovePropertyListenerBlock(watched_, &watched_address_, queue_, listener_);
        listener_ = nil;
    }

    /// On the device's realtime thread.
    static OSStatus render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp* time,
                           UInt32, UInt32 frames, AudioBufferList* data) {
        auto* self = static_cast<Stream*>(context);
        if (!data || data->mNumberBuffers == 0 || !data->mBuffers[0].mData) return noErr;
        const size_t samples = std::min<size_t>(size_t{frames} * self->options_.format.channels,
                                                data->mBuffers[0].mDataByteSize / sizeof(float));
        const Duration latency{self->latency_us_.load(std::memory_order_relaxed)};
        const Timestamp at = (time && (time->mFlags & kAudioTimeStampHostTimeValid))
                                 ? at_host_time(self->clock_, time->mHostTime) + latency
                                 : self->clock_.now() + latency;
        self->pull_({static_cast<float*>(data->mBuffers[0].mData), samples}, at);
        return noErr;
    }

    const AudioOutputOptions options_;
    Clock& clock_;
    const Pull pull_;
    const EndHandler ended_;
    dispatch_queue_t queue_;
    std::atomic<int64_t> latency_us_{0};

    // The queue's alone.
    AudioComponentInstance unit_ = nullptr;
    bool initialized_ = false;
    bool started_ = false;
    bool stopped_ = false;
    bool over_ = false;
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioObjectPropertyListenerBlock listener_ = nil;
    AudioObjectID watched_ = kAudioObjectUnknown;
    AudioObjectPropertyAddress watched_address_{};
};

AudioPlayback::AudioPlayback(const AudioOutputOptions& options, Clock& clock)
    : options_(options), clock_(clock) {}

AudioPlayback::~AudioPlayback() { stop(); }

Result<void> AudioPlayback::start(Pull pull, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (stream_) return fail(Errc::InvalidArgument);
    auto stream = std::make_unique<Stream>(options_, clock_, std::move(pull), std::move(ended));
    if (Result<void> started = stream->start(); !started) return started;
    stream_ = std::move(stream);
    return {};
}

void AudioPlayback::stop() {
    std::lock_guard lock(mutex_);
    stream_.reset();
}

// ── Devices ─────────────────────────────────────────────────────────────────

Result<std::vector<AudioDeviceInfo>> list_devices(bool input) {
    std::vector<AudioDeviceInfo> devices;
    for (const AudioObjectID device :
         object_list(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices))) {
        if (channel_count(device, scope_of(input)) == 0) continue;
        if (get<UInt32>(device, address(kAudioDevicePropertyIsHidden)).value_or(0) != 0) continue;
        AudioDeviceInfo info{.id = string_property(device, kAudioDevicePropertyDeviceUID)};
        if (info.id.empty() || info.id.starts_with(kOwnDevicePrefix)) continue;
        info.name = string_property(device, kAudioObjectPropertyName);
        if (info.name.empty()) info.name = input ? "Microphone" : "Speakers";
        devices.push_back(std::move(info));
    }
    return devices;
}

}  // namespace ratcast::core_audio
