#pragma once

/**
 * @file video_window.h
 * @brief A window that shows the newest picture it was given (SDL3).
 *
 * Pictures arrive from a decoder thread through show(); the window keeps only
 * the newest and draws it on the main thread, where run() loops — SDL wants
 * its windows driven from there. YUV pictures go to the GPU as they are, with
 * their colour space; SDL converts them while drawing.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "ratcast/engine/video_frame.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace ratcast::tools {

class VideoWindow {
public:
    /// A window titled `title`; nullptr, with the reason printed, when there
    /// is no display to open one on.
    static std::unique_ptr<VideoWindow> open(const std::string& title);
    ~VideoWindow();
    VideoWindow(const VideoWindow&) = delete;
    VideoWindow& operator=(const VideoWindow&) = delete;

    /// Show `picture` next, in place of any not yet drawn. Any thread.
    void show(VideoFrame picture);

    /// Draw pictures as they come until the window is closed or `tick`, which
    /// runs at least every 100 ms, returns false. Main thread.
    void run(const std::function<bool()>& tick);

private:
    VideoWindow() = default;
    void draw(const VideoFrame& picture);
    bool fit_texture(const VideoFrame& picture);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    uint32_t picture_event_ = 0;

    // What texture_ was made for.
    PixelFormat format_ = PixelFormat::NV12;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    ColorSpace color_space_;

    std::mutex mutex_;
    std::optional<VideoFrame> next_;
};

}  // namespace ratcast::tools
