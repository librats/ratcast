#pragma once

/**
 * @file preview.h
 * @brief What a source shows, seen by its sender as it goes.
 *
 * with_preview() hands each picture of a source to a preview as well as to its
 * publisher. A camera's small pictures may go to a view as they are. A
 * screen's would cost a view the upload of every one at full size — at 1440p
 * and 60 a second, 330 MB a second — to show them in a corner: a Preview takes
 * few of them and makes them small. At most `frame_rate` a second, each shrunk
 * by a whole factor to fit within `max_width` x `max_height` (shrink_frame():
 * the mean of each block, out of GPU memory first), on a thread of its own so
 * that the source never waits for it. While nothing looks (no sink), it does
 * nothing at all.
 */

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "ratcast/core/time.h"
#include "ratcast/engine/video_frame.h"
#include "ratcast/engine/video_source.h"

namespace ratcast {

/// `source`, each of its pictures handed to `preview` as well (on the
/// source's thread, before the publisher has it). Pictures are shared, not
/// copied.
std::unique_ptr<VideoSource> with_preview(std::unique_ptr<VideoSource> source,
                                          VideoSource::Sink preview);

struct PreviewConfig {
    /// The largest a picture is shown: a corner of a window, on a screen of
    /// twice the pixels.
    uint32_t max_width = 640;
    uint32_t max_height = 360;
    /// Pictures a second at most: a film seen to play.
    uint32_t frame_rate = 15;
};

class Preview {
public:
    explicit Preview(PreviewConfig config = {});
    ~Preview();
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    /// Where the pictures go from now on, on the preview's thread; empty:
    /// nowhere, and none is taken. Once this returns the sink before is not
    /// called again.
    void set_sink(VideoSource::Sink sink);

    /// A picture of the source, from its thread: kept when one is due — the
    /// newest only, until the preview's thread takes it — and passed over
    /// otherwise. Never waits for a picture to be made small.
    void take(const VideoFrame& picture);

private:
    void run(std::stop_token stop);

    const PreviewConfig config_;
    const Duration interval_;

    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool shown_ = false;  ///< there is a sink
    std::optional<VideoFrame> pending_;
    std::optional<Timestamp> due_;  ///< the capture time the next picture is kept from

    std::mutex sink_mutex_;  ///< held while the sink is called
    VideoSource::Sink sink_;

    std::jthread thread_;  ///< last: it stops before the rest goes
};

}  // namespace ratcast
