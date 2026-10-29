// The Java VM and Context the application gave: see android/java.h.

#include "android/java.h"

#include <pthread.h>

#include <atomic>

#include "ratcast/engine/android.h"

namespace ratcast::android {
namespace {

std::atomic<JavaVM*> vm_{nullptr};
std::atomic<jobject> context_{nullptr};

/// Threads attached here are detached as they end, which a thread still
/// attached may not do (the VM aborts).
pthread_key_t attached_key() {
    static const pthread_key_t key = [] {
        pthread_key_t made{};
        pthread_key_create(&made, [](void*) {
            if (JavaVM* vm = vm_.load()) vm->DetachCurrentThread();
        });
        return made;
    }();
    return key;
}

}  // namespace

void attach(JavaVM* vm, jobject context) {
    vm_.store(vm);
    JNIEnv* env = jni_env();
    if (env && context) context_.store(env->NewGlobalRef(context));
}

JNIEnv* jni_env() {
    JavaVM* vm = vm_.load();
    if (!vm) return nullptr;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(attached_key(), env);
    return env;
}

jobject jni_context() { return context_.load(); }

bool jni_failed(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

}  // namespace ratcast::android
