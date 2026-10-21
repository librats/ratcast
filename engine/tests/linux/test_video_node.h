#pragma once

/**
 * @file test_video_node.h
 * @brief A stand-in for a compositor's screen, or a camera, on the user's
 *        PipeWire daemon.
 *
 * A video source node that sends BGRx pictures in shared memory, the way
 * GNOME's and KDE's screen casts do, with the same metadata, and that can be
 * resized or taken away — what pipewire::Capture has to cope with, without a
 * compositor or a portal dialog in the way. Or one that says it is a camera
 * and sends what cameras send: YUY2, NV12 or I420, rows padded, planes one
 * after the other in a block or a block each.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "ratcast/engine/video_frame.h"

namespace ratcast::test {

class TestVideoNode {
public:
    struct Crop {
        int32_t x, y;
        uint32_t width, height;
    };

    struct Options {
        PixelFormat format = PixelFormat::BGRA;  ///< sent as BGRx
        bool camera = false;                     ///< a camera's role, not a screen's
        std::string name = "ratcast test screen";
        bool block_per_plane = false;
    };

    /// A node of `width` x `height`, ready to be connected to; nullptr when
    /// there is no PipeWire daemon (or it would not take the node).
    static std::unique_ptr<TestVideoNode> create(uint32_t width, uint32_t height, Options options);
    static std::unique_ptr<TestVideoNode> create(uint32_t width, uint32_t height) {
        return create(width, height, Options{});
    }
    ~TestVideoNode();

    uint32_t node_id() const noexcept;

    /// Send `picture` — in the node's format and size — with crop metadata if
    /// `crop`.
    /// Dropped when no one is connected or every buffer is taken.
    void show(const VideoFrame& picture, std::optional<Crop> crop = std::nullopt);

    /// Change the node's size, as a resized window does.
    void resize(uint32_t width, uint32_t height);

    class Impl;

private:
    explicit TestVideoNode(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ratcast::test
