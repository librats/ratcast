#pragma once

/**
 * @file java.h
 * @brief Java from the engine's threads: what attach() was given.
 */

#include <jni.h>

namespace ratcast::android {

/// The calling thread's JNIEnv, the thread attached to the VM if it was not
/// (and detached as it ends); null when no VM was given.
JNIEnv* jni_env();

/// The Context attach() was given (a global reference), or null.
jobject jni_context();

/// Whether the last call threw; the exception is cleared.
bool jni_failed(JNIEnv* env);

}  // namespace ratcast::android
