#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace p2premote {

inline constexpr std::uint32_t audio_sample_rate = 48'000;
inline constexpr std::uint16_t audio_channels = 1;
inline constexpr std::uint16_t audio_bits_per_sample = 16;
inline constexpr std::size_t audio_frame_samples = 480; // 10 ms at 48 kHz
inline constexpr std::size_t audio_frame_bytes = audio_frame_samples * sizeof(std::int16_t); // 960 bytes

class AudioRingBuffer {
public:
    explicit AudioRingBuffer(std::size_t capacity_samples = audio_frame_samples * 12,
        std::size_t prebuffer_samples = audio_frame_samples * 2,
        std::size_t trim_target_samples = audio_frame_samples * 4);

    void Push(const std::int16_t* samples, std::size_t count);
    std::size_t Pop(std::int16_t* destination, std::size_t count, float volume = 1.0f);
    void Clear();
    std::size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::int16_t> buffer_;
    std::size_t read_pos_{};
    std::size_t write_pos_{};
    std::size_t count_{};
    std::size_t prebuffer_samples_;
    std::size_t trim_target_samples_;
    bool primed_{};
};

class AudioCapture {
public:
    enum class Mode { desktop_loopback, microphone };
    using FrameCallback = std::function<void(const char* data, std::size_t size)>;

    AudioCapture(Mode mode, FrameCallback on_frame);
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    void SetPaused(bool paused) noexcept;
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class AudioPlayer {
public:
    enum class Target { default_speaker, virtual_mic_or_speaker };

    AudioPlayer(Target target, bool allow_speaker_fallback = true);
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    void PushPcm(const char* data, std::size_t size);
    void SetMuted(bool muted) noexcept;
    void SetVolume(float volume) noexcept;
    bool UsingVirtualMic() const noexcept;
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace p2premote
