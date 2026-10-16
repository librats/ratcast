#pragma once

/**
 * @file test_video_node.h
 * @brief A stand-in for a compositor's screen, on the user's PipeWire daemon.
 *
 * A video source node that sends BGRx pictures in shared memory, the way
 * GNOME's and KDE's screen casts do, with the same metadata, and that can be
 * resized or taken away — what pipewire::Capture has to cope with, without a
 * compositor or a portal dialog in the way.
 */

#include <cstdint>
#include <memory>
#include <optional>

#include "ratcast/engine/video_frame.h"

namespace ratcast::test {

class TestVideoNode {
public:
    struct Crop {
        int32_t x, y;
        uint32_t width, height;
    };

    /// A node of `width` x `height`, ready to be connected to; nullptr when
    /// there is no PipeWire daemon (or it would not take the node).
    static std::unique_ptr<TestVideoNode> create(uint32_t width, uint32_t height);
    ~TestVideoNode();

    uint32_t node_id() const noexcept;

    /// Send `picture` — BGRA, the node's size — with crop metadata if `crop`.
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
