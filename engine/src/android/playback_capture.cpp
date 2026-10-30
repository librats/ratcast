// What other applications play on Android: see android/playback_capture.h.

#include "android/playback_capture.h"

#include <utility>

#include "android/java.h"
#include "android/projection.h"
#include "audio/sample_clock.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

/// AudioRecord.READ_BLOCKING: a read waits until it has all it asked for.
constexpr jint kReadBlocking = 0;
/// AudioRecord.ERROR_DEAD_OBJECT: the record went, with its projection.
constexpr jint kDeadObject = -6;

void call_void(JNIEnv* env, jobject object, const char* name) {
    jclass type = env->GetObjectClass(object);
    env->CallVoidMethod(object, env->GetMethodID(type, name, "()V"));
    env->DeleteLocalRef(type);
    jni_failed(env);
}

}  // namespace

PlaybackCapture::PlaybackCapture(const AudioCaptureOptions& options, Clock& clock)
    : format_(options.format), clock_(clock) {}

PlaybackCapture::~PlaybackCapture() { stop(); }

Result<void> PlaybackCapture::start(Sink sink, EndHandler ended) {
    std::lock_guard lock(mutex_);
    if (record_) return fail(Errc::InvalidArgument);
    std::shared_ptr<Projection> projection = Projection::current();
    if (!projection) {
        LOG_WARN("engine", "the applications' sound is heard only beside the screen shared");
        return fail(Errc::Unavailable);
    }
    jobject record = projection->sound(format_.sample_rate, format_.channels);
    if (!record) return fail(Errc::Unavailable);
    projection_ = std::move(projection);
    record_ = record;
    stopping_ = false;
    reader_ = std::thread(&PlaybackCapture::read, this, std::move(sink), std::move(ended));
    return {};
}

void PlaybackCapture::stop() {
    std::lock_guard lock(mutex_);
    if (!record_) return;
    JNIEnv* env = jni_env();
    stopping_ = true;
    // A read waiting for more returns with what it has.
    if (env) call_void(env, record_, "stop");
    if (reader_.joinable()) reader_.join();
    if (env) {
        call_void(env, record_, "release");
        env->DeleteGlobalRef(record_);
    }
    record_ = nullptr;
    projection_.reset();
}

/// The reader's thread: 10 ms at a time, until stopped.
void PlaybackCapture::read(Sink sink, EndHandler ended) {
    JNIEnv* env = jni_env();
    if (!env) return;
    const size_t block = format_.sample_rate / 100;
    const jint samples = jint(block * format_.channels);
    jfloatArray buffer = env->NewFloatArray(samples);
    jclass type = env->GetObjectClass(record_);
    const jmethodID read = env->GetMethodID(type, "read", "([FIII)I");
    env->DeleteLocalRef(type);
    SampleClock sample_clock(format_.sample_rate);
    while (!stopping_) {
        const jint got = env->CallIntMethod(record_, read, buffer, 0, samples, kReadBlocking);
        if (jni_failed(env) || got < 0) {
            if (stopping_) break;
            LOG_WARN("engine", "the applications' sound ended: " << got);
            if (ended) ended(got == kDeadObject ? Result<void>{} : fail(Errc::Internal));
            break;
        }
        const size_t length = size_t(got) / format_.channels;
        if (length == 0) continue;
        MutableAudioFrame frame = MutableAudioFrame::allocate(format_, length);
        env->GetFloatArrayRegion(buffer, 0, jint(length * format_.channels),
                                 frame.samples().data());
        sink(std::move(frame).freeze(sample_clock.stamp(length, clock_.now())));
    }
    env->DeleteLocalRef(buffer);
}

}  // namespace ratcast::android
