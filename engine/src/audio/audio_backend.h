#pragma once

/**
 * @file audio_backend.h
 * @brief What an audio codec backend provides behind audio_codec.h.
 *
 * The public entry points check their arguments and hand valid requests here,
 * so every backend — libopus, or none at all — answers the same way to the
 * same mistakes.
 */

#include <memory>

#include "ratcast/core/result.h"
#include "ratcast/engine/audio_codec.h"

namespace ratcast::backend {

/// Precondition: `config` passed open_audio_encoder()'s checks.
Result<std::unique_ptr<AudioEncoder>> open_audio_encoder(const AudioEncoderConfig& config);

/// Precondition: `config` passed open_audio_decoder()'s checks.
Result<std::unique_ptr<AudioDecoder>> open_audio_decoder(const AudioDecoderConfig& config);

}  // namespace ratcast::backend
