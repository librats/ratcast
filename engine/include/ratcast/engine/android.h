#pragma once

/**
 * @file android.h
 * @brief What the engine needs on Android of the application it runs in.
 *
 * Some of Android's media is reached only through Java — keeping the screen
 * on, and later the screen's capture and other applications' sound — so the
 * engine needs the process's Java VM and a Context. An application hands them
 * over as it starts (the app through Qt); a program with no Java, as the
 * tools are under `adb shell`, does not, and those parts say Unavailable or
 * do nothing there. Android only.
 */

#include <jni.h>

namespace ratcast::android {

/// Before anything that needs Java. `context` is kept (a global reference)
/// for the process's life; the application's own Context is the one to give.
void attach(JavaVM* vm, jobject context);

}  // namespace ratcast::android
