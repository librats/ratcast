#include "recv/video_window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <iostream>
#include <utility>

namespace ratcast::tools {
namespace {

SDL_PixelFormat sdl_format(PixelFormat format) {
    switch (format) {
        case PixelFormat::I420: return SDL_PIXELFORMAT_IYUV;
        case PixelFormat::NV12: return SDL_PIXELFORMAT_NV12;
        case PixelFormat::BGRA: return SDL_PIXELFORMAT_BGRA32;
    }
    return SDL_PIXELFORMAT_UNKNOWN;
}

SDL_Colorspace sdl_colorspace(PixelFormat format, ColorSpace color_space) {
    if (format == PixelFormat::BGRA) return SDL_COLORSPACE_SRGB;
    const bool full = color_space.range == ColorRange::Full;
    if (color_space.matrix == ColorMatrix::BT601) {
        return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    }
    return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
}

}  // namespace

std::unique_ptr<VideoWindow> VideoWindow::open(const std::string& title) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  // Ctrl-C is the tool's
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "no window: " << SDL_GetError() << "\n";
        return nullptr;
    }
    std::unique_ptr<VideoWindow> window(new VideoWindow());
    if (!SDL_CreateWindowAndRenderer(title.c_str(), 1280, 720, SDL_WINDOW_RESIZABLE,
                                     &window->window_, &window->renderer_)) {
        std::cerr << "no window: " << SDL_GetError() << "\n";
        return nullptr;
    }
    // Where it cannot wait for the refresh, presenting returns at once.
    SDL_SetRenderVSync(window->renderer_, 1);
    window->picture_event_ = SDL_RegisterEvents(1);
    SDL_SetRenderDrawColor(window->renderer_, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(window->renderer_);
    SDL_RenderPresent(window->renderer_);
    return window;
}

VideoWindow::~VideoWindow() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

void VideoWindow::show(VideoFrame picture) {
    bool first = false;
    {
        std::lock_guard lock(mutex_);
        first = !next_.has_value();
        next_ = std::move(picture);
    }
    if (first) {  // one wake-up per picture drawn is enough
        SDL_Event event{};
        event.type = picture_event_;
        SDL_PushEvent(&event);
    }
}

void VideoWindow::run(const std::function<bool()>& tick, const Presented& presented) {
    for (;;) {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 100)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_Q) return;
                    break;
                default: break;
            }
        }
        std::optional<VideoFrame> picture;
        {
            std::lock_guard lock(mutex_);
            picture = std::exchange(next_, std::nullopt);
        }
        if (picture && draw(*picture) && presented) presented(*picture);
        if (!tick()) return;
    }
}

bool VideoWindow::draw(const VideoFrame& picture) {
    if (!fit_texture(picture)) return false;
    const auto stride = [&](size_t i) { return static_cast<int>(picture.plane(i).stride); };
    switch (picture.format()) {
        case PixelFormat::NV12:
            SDL_UpdateNVTexture(texture_, nullptr, picture.plane(0).data, stride(0),
                                picture.plane(1).data, stride(1));
            break;
        case PixelFormat::I420:
            SDL_UpdateYUVTexture(texture_, nullptr, picture.plane(0).data, stride(0),
                                 picture.plane(1).data, stride(1), picture.plane(2).data,
                                 stride(2));
            break;
        case PixelFormat::BGRA:
            SDL_UpdateTexture(texture_, nullptr, picture.plane(0).data, stride(0));
            break;
    }
    SDL_RenderClear(renderer_);
    SDL_RenderTexture(renderer_, texture_, nullptr, nullptr);
    return SDL_RenderPresent(renderer_);
}

/// A texture of the picture's format, size and colours, made anew on a change.
bool VideoWindow::fit_texture(const VideoFrame& picture) {
    if (texture_ && picture.format() == format_ && picture.width() == width_ &&
        picture.height() == height_ && picture.color_space() == color_space_) {
        return true;
    }
    if (texture_) SDL_DestroyTexture(texture_);
    const bool first = texture_ == nullptr;

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                          sdl_format(picture.format()));
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, picture.width());
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, picture.height());
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                          sdl_colorspace(picture.format(), picture.color_space()));
    texture_ = SDL_CreateTextureWithProperties(renderer_, props);
    SDL_DestroyProperties(props);
    if (!texture_) {
        std::cerr << "cannot draw " << picture.width() << "x" << picture.height() << ": "
                  << SDL_GetError() << "\n";
        return false;
    }
    format_ = picture.format();
    width_ = picture.width();
    height_ = picture.height();
    color_space_ = picture.color_space();

    // Keep the picture's shape whatever the window's; size the window to the
    // first picture, within reason.
    SDL_SetRenderLogicalPresentation(renderer_, static_cast<int>(width_), static_cast<int>(height_),
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
    if (first) {
        const float scale = std::min({1.0f, 1600.0f / float(width_), 900.0f / float(height_)});
        SDL_SetWindowSize(window_, static_cast<int>(float(width_) * scale),
                          static_cast<int>(float(height_) * scale));
    }
    return true;
}

}  // namespace ratcast::tools
