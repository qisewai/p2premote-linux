#pragma once

#include <p2premote/input_protocol.h>
#include <string>

namespace p2premote::linux_host {

class InputSink {
public:
    virtual ~InputSink() = default;
    virtual bool IsAvailable() const noexcept = 0;
    virtual void WakeDisplay() noexcept = 0;
    virtual void Apply(const InputPacket& packet) noexcept = 0;
    virtual void ApplyCommand(const InputCommand& command) noexcept = 0;
    virtual void ReleaseAll() noexcept = 0;
    virtual std::string Describe() const = 0;
};

} // namespace p2premote::linux_host
