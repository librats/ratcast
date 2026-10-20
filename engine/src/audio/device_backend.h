#pragma once

/**
 * @file device_backend.h
 * @brief What a sound device backend provides behind audio_device.h.
 *
 * The public entry points check their arguments and hand valid requests here,
 * so every backend — PipeWire, or none at all — answers the same way to the
 * same mistakes.
 */

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/audio_device.h"

namespace ratcast::backend {

/// Precondition: `options` passed open_audio_capture()'s checks.
Result<std::unique_ptr<AudioSource>> open_audio_capture(const AudioCaptureOptions& options);

/// Precondition: `options` passed open_audio_output()'s checks.
Result<std::unique_ptr<AudioOutput>> open_audio_output(const AudioOutputOptions& options);

}  // namespace ratcast::backend
