// A call on Android, told the system through Java's InCall (java/ratcast/
// engine/InCall.java): the audio mode, routing and focus, and the foreground
// service. See in_call.h.

#include "ratcast/engine/in_call.h"

#include "android/java.h"

#include "librats/util/logger.h"

namespace ratcast {
namespace {

class JavaCall final : public InCall {
public:
    JavaCall() {
        JNIEnv* env = android::jni_env();
        if (!env || !android::jni_context()) {
            LOG_WARN("engine", "the system is not told of the call: no Java here");
            return;
        }
        jclass type = android::jni_class(env, "ratcast.engine.InCall");
        if (!type) {
            LOG_WARN("engine", "the system is not told of the call: no ratcast.engine.InCall");
            return;
        }
        jobject made =
            env->NewObject(type, env->GetMethodID(type, "<init>", "(Landroid/content/Context;)V"),
                           android::jni_context());
        if (!android::jni_failed(env) && made) {
            call_ = env->NewGlobalRef(made);
            env->DeleteLocalRef(made);
        } else {
            LOG_WARN("engine", "the system is not told of the call: it refused");
        }
        env->DeleteLocalRef(type);
    }
    JavaCall(const JavaCall&) = delete;
    JavaCall& operator=(const JavaCall&) = delete;
    ~JavaCall() override {
        if (!call_) return;
        JNIEnv* env = android::jni_env();
        if (!env) return;
        invoke(env, "leave");
        env->DeleteGlobalRef(call_);
    }

    void update() override {
        if (!call_) return;
        if (JNIEnv* env = android::jni_env()) invoke(env, "update");
    }

private:
    void invoke(JNIEnv* env, const char* method) {
        jclass type = env->GetObjectClass(call_);
        env->CallVoidMethod(call_, env->GetMethodID(type, method, "()V"));
        if (android::jni_failed(env)) LOG_WARN("engine", "the call's " << method << " failed");
        env->DeleteLocalRef(type);
    }

    jobject call_ = nullptr;  ///< InCall's, a global reference
};

}  // namespace

std::unique_ptr<InCall> enter_call() { return std::make_unique<JavaCall>(); }

}  // namespace ratcast
