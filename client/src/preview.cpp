#include "ratcast/client/preview.h"

#include <algorithm>
#include <utility>

#include "ratcast/engine/video_convert.h"

namespace ratcast {
namespace {

class Previewed final : public VideoSource {
public:
    Previewed(std::unique_ptr<VideoSource> source, Sink preview)
        : source_(std::move(source)), preview_(std::move(preview)) {}
    ~Previewed() override { stop(); }

    Result<void> start(Sink sink, EndHandler ended) override {
        return source_->start(
            [this, sink = std::move(sink)](VideoFrame picture) {
                if (preview_) preview_(picture);
                sink(std::move(picture));
            },
            std::move(ended));
    }
    void stop() override { source_->stop(); }

private:
    std::unique_ptr<VideoSource> source_;
    const Sink preview_;
};

}  // namespace

std::unique_ptr<VideoSource> with_preview(std::unique_ptr<VideoSource> source,
                                          VideoSource::Sink preview) {
    return std::make_unique<Previewed>(std::move(source), std::move(preview));
}

Preview::Preview(PreviewConfig config)
    : config_(config),
      interval_(Duration(std::chrono::seconds(1)) / std::max(config.frame_rate, 1u)),
      thread_([this](std::stop_token stop) { run(stop); }) {}

// The thread, stopped first, wakes: wake_ waits on its stop token.
Preview::~Preview() = default;

void Preview::set_sink(VideoSource::Sink sink) {
    {
        std::lock_guard lock(mutex_);
        shown_ = static_cast<bool>(sink);
        if (!shown_) pending_.reset();
    }
    std::lock_guard lock(sink_mutex_);
    sink_ = std::move(sink);
}

void Preview::take(const VideoFrame& picture) {
    {
        std::lock_guard lock(mutex_);
        if (!shown_) return;
        // Due by the capture times, so that the rate holds whatever the
        // source's; one from long before the last (another source, on
        // another clock) starts them again.
        const Timestamp time = picture.capture_time();
        if (due_ && time + interval_ < *due_) due_.reset();
        if (due_ && time < *due_) return;
        due_ = due_ ? *due_ + interval_ : time;
        if (*due_ <= time) due_ = time + interval_;  // after a gap: from now
        pending_ = picture;
    }
    wake_.notify_one();
}

void Preview::run(std::stop_token stop) {
    for (;;) {
        VideoFrame picture;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [&] { return pending_.has_value(); })) return;
            picture = std::move(*pending_);
            pending_.reset();
        }
        const uint32_t factor =
            shrink_factor(picture.width(), picture.height(), config_.max_width, config_.max_height);
        Result<VideoFrame> small = shrink_frame(picture, factor);
        picture = {};          // the source's buffer back to it at once
        if (!small) continue;  // a format of no 4:2:0: nothing shown
        std::lock_guard lock(sink_mutex_);
        if (sink_) sink_(std::move(*small));
    }
}

}  // namespace ratcast
