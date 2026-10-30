// The screen shared on Android, through Java's ScreenShare: see
// android/projection.h.

#include "android/projection.h"

#include <android/native_window_jni.h>

#include <mutex>

#include "android/java.h"

#include "librats/util/logger.h"

namespace ratcast::android {
namespace {

/// How long each wait for the user's answer lasts, `stop` looked at between.
constexpr jlong kAnswerWaitMillis = 100;

std::mutex current_mutex;
std::weak_ptr<Projection> current_projection;

jmethodID method(JNIEnv* env, jobject object, const char* name, const char* signature) {
    jclass type = env->GetObjectClass(object);
    jmethodID id = env->GetMethodID(type, name, signature);
    env->DeleteLocalRef(type);
    return id;
}

}  // namespace

Result<std::shared_ptr<Projection>> Projection::ask(std::stop_token stop) {
    JNIEnv* env = jni_env();
    if (!env || !jni_context()) {
        LOG_WARN("engine", "the screen cannot be shared: no Java here");
        return fail(Errc::Unavailable);
    }
    jclass type = jni_class(env, "ratcast.engine.ScreenShare");
    if (!type) {
        LOG_WARN("engine", "the screen cannot be shared: no ratcast.engine.ScreenShare");
        return fail(Errc::Unavailable);
    }
    jobject made = env->NewObject(
        type, env->GetMethodID(type, "<init>", "(Landroid/content/Context;)V"), jni_context());
    env->DeleteLocalRef(type);
    if (jni_failed(env) || !made) return fail(Errc::Unavailable);
    jobject share = env->NewGlobalRef(made);
    env->DeleteLocalRef(made);
    const auto given_up = [&](Errc error) {
        env->DeleteGlobalRef(share);
        return fail(error);
    };

    if (!env->CallBooleanMethod(share, method(env, share, "ask", "()Z")) || jni_failed(env))
        return given_up(Errc::Unavailable);
    const jmethodID answer = method(env, share, "answer", "(J)I");
    for (;;) {
        const jint said = env->CallIntMethod(share, answer, kAnswerWaitMillis);
        if (jni_failed(env)) return given_up(Errc::Unavailable);
        if (said > 0) break;
        if (said == 0) {
            LOG_INFO("engine", "the screen was not allowed");
            return given_up(Errc::Cancelled);
        }
        if (stop.stop_requested()) {
            env->CallVoidMethod(share, method(env, share, "cancel", "()V"));
            jni_failed(env);
            return given_up(Errc::Cancelled);
        }
    }
    if (!env->CallBooleanMethod(share, method(env, share, "project", "()Z")) || jni_failed(env)) {
        env->CallVoidMethod(share, method(env, share, "close", "()V"));
        jni_failed(env);
        return given_up(Errc::Unavailable);
    }
    auto projection = std::make_shared<Projection>(share);
    std::lock_guard lock(current_mutex);
    current_projection = projection;
    return projection;
}

std::shared_ptr<Projection> Projection::current() {
    std::lock_guard lock(current_mutex);
    return current_projection.lock();
}

Projection::~Projection() {
    JNIEnv* env = jni_env();
    if (!env) return;
    env->CallVoidMethod(share_, method(env, share_, "close", "()V"));
    if (jni_failed(env)) LOG_WARN("engine", "the projection did not close");
    env->DeleteGlobalRef(share_);
}

std::optional<Projection::Screen> Projection::screen() const {
    JNIEnv* env = jni_env();
    if (!env) return std::nullopt;
    auto sizes = static_cast<jintArray>(
        env->CallObjectMethod(share_, method(env, share_, "screen", "()[I")));
    if (jni_failed(env) || !sizes) return std::nullopt;
    jint values[3] = {};
    if (env->GetArrayLength(sizes) >= 3) env->GetIntArrayRegion(sizes, 0, 3, values);
    env->DeleteLocalRef(sizes);
    if (values[0] <= 0 || values[1] <= 0) return std::nullopt;
    return Screen{uint32_t(values[0]), uint32_t(values[1]), values[2]};
}

bool Projection::show(ANativeWindow* window, uint32_t width, uint32_t height, int32_t dpi) const {
    JNIEnv* env = jni_env();
    if (!env) return false;
    jobject surface = ANativeWindow_toSurface(env, window);
    if (!surface) return false;
    const jboolean shown =
        env->CallBooleanMethod(share_, method(env, share_, "show", "(Landroid/view/Surface;III)Z"),
                               surface, jint(width), jint(height), jint(dpi));
    env->DeleteLocalRef(surface);
    return !jni_failed(env) && shown;
}

void Projection::hide() const {
    JNIEnv* env = jni_env();
    if (!env) return;
    env->CallVoidMethod(share_, method(env, share_, "hide", "()V"));
    jni_failed(env);
}

bool Projection::stopped() const {
    JNIEnv* env = jni_env();
    if (!env) return true;
    const jboolean stopped = env->CallBooleanMethod(share_, method(env, share_, "stopped", "()Z"));
    return jni_failed(env) || stopped;
}

jobject Projection::sound(uint32_t sample_rate, uint32_t channels) const {
    JNIEnv* env = jni_env();
    if (!env) return nullptr;
    jobject record = env->CallObjectMethod(
        share_, method(env, share_, "sound", "(II)Landroid/media/AudioRecord;"), jint(sample_rate),
        jint(channels));
    if (jni_failed(env) || !record) return nullptr;
    jobject held = env->NewGlobalRef(record);
    env->DeleteLocalRef(record);
    return held;
}

}  // namespace ratcast::android
