// The screen kept on by a wake lock of PowerManager's that keeps it bright:
// taken from any thread (a window's FLAG_KEEP_SCREEN_ON wants the UI's), and
// granted at once. Needs WAKE_LOCK, and the Context attach() was given.

#include "ratcast/engine/screen_awake.h"

#include <string>

#include "android/java.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

// PowerManager's: the screen kept bright, and left on a while after release
// as a touch would.
constexpr jint kScreenBrightWakeLock = 0x0000000a;
constexpr jint kOnAfterRelease = 0x20000000;

class WakeLockAwake final : public ScreenAwake {
public:
    explicit WakeLockAwake(const std::string& reason) {
        JNIEnv* env = android::jni_env();
        jobject context = android::jni_context();
        if (!env || !context) {
            LOG_WARN("engine", "the screen may blank: no Java here to keep it on");
            return;
        }
        lock_ = make(env, context, "ratcast:" + reason);
        if (!lock_) {
            LOG_WARN("engine", "the screen may blank: no wake lock");
            return;
        }
        jclass type = env->GetObjectClass(lock_);
        env->CallVoidMethod(lock_, env->GetMethodID(type, "acquire", "()V"));
        held_ = !android::jni_failed(env);
        env->DeleteLocalRef(type);
        if (!held_) LOG_WARN("engine", "the screen may blank: the wake lock was refused");
    }
    WakeLockAwake(const WakeLockAwake&) = delete;
    WakeLockAwake& operator=(const WakeLockAwake&) = delete;
    ~WakeLockAwake() override {
        if (!lock_) return;
        JNIEnv* env = android::jni_env();
        if (!env) return;
        if (held_) {
            jclass type = env->GetObjectClass(lock_);
            env->CallVoidMethod(lock_, env->GetMethodID(type, "release", "()V"));
            android::jni_failed(env);
            env->DeleteLocalRef(type);
        }
        env->DeleteGlobalRef(lock_);
    }

    bool held() const noexcept override { return held_; }

private:
    /// context.getSystemService("power").newWakeLock(...), as a global
    /// reference; null if any of it fails.
    static jobject make(JNIEnv* env, jobject context, const std::string& tag) {
        jobject lock = nullptr;
        jclass context_type = env->GetObjectClass(context);
        jstring service = env->NewStringUTF("power");
        jobject power =
            env->CallObjectMethod(context,
                                  env->GetMethodID(context_type, "getSystemService",
                                                   "(Ljava/lang/String;)Ljava/lang/Object;"),
                                  service);
        if (!android::jni_failed(env) && power) {
            jclass power_type = env->GetObjectClass(power);
            jstring name = env->NewStringUTF(tag.c_str());
            jobject made = env->CallObjectMethod(
                power,
                env->GetMethodID(power_type, "newWakeLock",
                                 "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;"),
                kScreenBrightWakeLock | kOnAfterRelease, name);
            if (!android::jni_failed(env) && made) {
                lock = env->NewGlobalRef(made);
                env->DeleteLocalRef(made);
            }
            env->DeleteLocalRef(name);
            env->DeleteLocalRef(power_type);
            env->DeleteLocalRef(power);
        }
        env->DeleteLocalRef(service);
        env->DeleteLocalRef(context_type);
        return lock;
    }

    jobject lock_ = nullptr;
    bool held_ = false;
};

}  // namespace

std::unique_ptr<ScreenAwake> keep_screen_awake(std::string reason) {
    return std::make_unique<WakeLockAwake>(reason);
}

}  // namespace ratcast
