#pragma once

/**
 * @file android.h
 * @brief What the engine needs on Android of the application it runs in, and
 * what it hands the application's own drawing.
 *
 * Some of Android's media is reached only through Java — keeping the screen
 * on, the screen's capture and other applications' sound — so the engine
 * needs the process's Java VM and a Context. An application hands them over
 * as it starts (the app through Qt); a program with no Java, as the tools are
 * under `adb shell`, does not, and those parts say Unavailable or do nothing
 * there.
 *
 * Pictures in Android's GPU memory (GpuApi::HardwareBuffer: the screen's, a
 * decoder's asked for with VideoDecoderConfig::gpu) are AHardwareBuffers,
 * which a view draws where they are through an EGLImage of the buffer, a
 * texture of OpenGL ES's external kind that turns YUV into colour itself.
 * Android only.
 */

#include <jni.h>

#include "ratcast/engine/video_frame.h"

struct AHardwareBuffer;

namespace ratcast::android {

/// Before anything that needs Java. `context` is kept (a global reference)
/// for the process's life; the application's own Context is the one to give.
void attach(JavaVM* vm, jobject context);

/// The buffer a picture in Android's GPU memory is in, which lives as long as
/// the picture does; null for any other picture. Its picture is the buffer's
/// `width()` x `height()` from the top left.
AHardwareBuffer* hardware_buffer_of(const VideoFrame& picture) noexcept;

}  // namespace ratcast::android
