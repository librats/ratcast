#pragma once

/**
 * @file plain_text.h
 * @brief What a peer may put into words others are shown: UTF-8, nothing a
 * screen would act on.
 */

#include <string_view>

namespace ratcast {

/// Whether `text` is valid UTF-8 — no overlong form, no surrogate — without
/// control characters; with `lines`, line breaks and tabs allowed.
bool is_plain_text(std::string_view text, bool lines = false) noexcept;

}  // namespace ratcast
