#pragma once

/**
 * @file file_source.h
 * @brief Pictures from a video file, played at the file's own pace.
 *
 * For machines without a screen to capture, for tests that need real footage,
 * and as the start of watching a film together. Any container and codec the
 * build's FFmpeg reads; only the video is played, its first stream.
 *
 * Pictures come in the format asked for, keeping the file's colour space,
 * stamped with the time they were handed out — the moment a capture would
 * have taken them. A file that decodes slower than it plays falls behind
 * rather than bursting to catch up.
 */

#include <cstdint>
#include <memory>
#include <string>

#include "ratcast/core/result.h"
#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

struct FileSourceOptions {
    std::string path;
    PixelFormat format = PixelFormat::NV12;
    bool loop = true;  ///< start over at the end, instead of ending
};

class FileSource : public VideoSource {
public:
    struct Info {
        uint32_t width = 0;  ///< of the pictures it gives
        uint32_t height = 0;
        uint32_t frame_rate_num = 0;  ///< the file's nominal rate; 0 if it has none
        uint32_t frame_rate_den = 1;
    };

    virtual const Info& info() const noexcept = 0;
};

/// Open `options.path` and get ready to play it from the start; each start()
/// starts from the beginning again. Fails with Unavailable when the file
/// cannot be read or this build has no decoder for it, Unsupported when it
/// is not a video FFmpeg understands.
Result<std::unique_ptr<FileSource>> open_file_source(const FileSourceOptions& options,
                                                     Clock& clock = SteadyClock::instance());

}  // namespace ratcast
