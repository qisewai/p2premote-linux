#pragma once

#include <QByteArray>

#include <functional>
#include <memory>

class ViewerAudio {
public:
    using Input = std::function<void(QByteArray)>;

    explicit ViewerAudio(Input input);
    ~ViewerAudio();
    void configure(bool speaker, bool mic, float volume);
    void play(QByteArray pcm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
