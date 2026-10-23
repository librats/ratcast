#pragma once

#include <cmath>
#include <span>

namespace ratcast {

/// Louder than this is someone there: -45 dBFS, as RMS.
inline constexpr float kAudible = 0.0056f;

/// Whether `samples` are louder than kAudible.
inline bool audible(std::span<const float> samples) noexcept {
    if (samples.empty()) return false;
    double sum = 0;
    for (float s : samples) sum += double(s) * s;
    return std::sqrt(sum / double(samples.size())) > kAudible;
}

}  // namespace ratcast
