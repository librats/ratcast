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

/// The application's class `name` ("ratcast.engine.InCall"), as a local
/// reference, through the Context's class loader: FindClass on a thread
/// attached here sees the system's classes only. Null if there is no
/// Context or no such class.
jclass jni_class(JNIEnv* env, const char* name);

}  // namespace ratcast::android
