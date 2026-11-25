#pragma once

#include <QByteArray>

#include <cstddef>
#include <functional>
#include <memory>

class ViewerAudio final {
public:
    using Input = std::function<void(QByteArray)>;

    explicit ViewerAudio(Input input);
    ~ViewerAudio();

    void configure(bool speaker, bool microphone, float volume);
    void play(const char* pcm, std::size_t size);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
