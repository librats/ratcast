#pragma once

/**
 * @file test_audio_node.h
 * @brief A stand-in for a sound device on the user's PipeWire daemon.
 *
 * A node that says it is a device — a source, a microphone that sends a tone,
 * or a sink, speakers that hear what is played to them — listed with the
 * system's own, and linked to a stream that asks for it by name.
 */

#include <cstdint>
#include <memory>
#include <string>

namespace ratcast::test {

class TestAudioNode {
public:
    enum class Kind {
        Source,  ///< "Audio/Source": sends a 440 Hz tone of amplitude `kLevel`
        Sink,    ///< "Audio/Sink": hears
    };
    static constexpr float kLevel = 0.25f;

    /// A mono node at 48 kHz, named `name` (node.name and its description);
    /// nullptr when there is no PipeWire daemon (or it would not take it).
    static std::unique_ptr<TestAudioNode> create(Kind kind, const std::string& name);
    ~TestAudioNode();

    uint32_t node_id() const noexcept;
    /// A sink's: the loudest sample played to it yet.
    float loudest() const noexcept;

    class Impl;

private:
    explicit TestAudioNode(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ratcast::test
