#include "audio.h"
#include "audio_buffer.h"

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

constexpr size_t sample_frame_bytes = PcmRingBuffer::channels * sizeof(std::int16_t);
constexpr qsizetype frame_bytes = static_cast<qsizetype>(PcmRingBuffer::frame_samples * sizeof(std::int16_t));
constexpr pa_sample_spec sample_spec{PA_SAMPLE_S16LE, 48'000, static_cast<uint8_t>(PcmRingBuffer::channels)};
constexpr auto default_value = static_cast<uint32_t>(-1);

void closeStream(pa_stream*& stream) {
    if (!stream) return;
    pa_stream_set_write_callback(stream, nullptr, nullptr);
    pa_stream_set_read_callback(stream, nullptr, nullptr);
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    stream = nullptr;
}

struct PulseLock {
    pa_threaded_mainloop* loop;
    explicit PulseLock(pa_threaded_mainloop* value) : loop(value) { pa_threaded_mainloop_lock(loop); }
    ~PulseLock() { pa_threaded_mainloop_unlock(loop); }
};

} // namespace

struct ViewerAudio::Impl {
    explicit Impl(Input callback) : input(std::move(callback)) {
        loop = pa_threaded_mainloop_new();
        if (!loop) return;
        context = pa_context_new(pa_threaded_mainloop_get_api(loop), "P2P Remote");
        if (!context) return;
        pa_context_set_state_callback(context, [](pa_context* source, void* data) {
            if (pa_context_get_state(source) == PA_CONTEXT_READY) static_cast<Impl*>(data)->syncStreams();
        }, this);
        if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) == 0) {
            running = pa_threaded_mainloop_start(loop) == 0;
        }
    }

    ~Impl() {
        if (!loop) return;
        if (running) pa_threaded_mainloop_stop(loop);
        closeStream(playback);
        closeStream(recording);
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
        }
        pa_threaded_mainloop_free(loop);
    }

    void syncStreams() {
        if (pa_context_get_state(context) != PA_CONTEXT_READY) return;
        if (!speaker) {
            closeStream(playback);
            playback_ring.clear();
        } else if (!playback) {
            playback = pa_stream_new(context, "Remote desktop audio", &sample_spec, nullptr);
            if (playback) {
                pa_stream_set_write_callback(playback, [](pa_stream* stream, size_t nbytes, void* ctx) {
                    static_cast<Impl*>(ctx)->writePlayback(stream, nbytes);
                }, this);
                // Let PipeWire/PulseAudio keep the sink's native quantum (critical for Bluetooth)
                // while pulling continuously from the jitter-absorbing ring buffer.
                const pa_buffer_attr attributes{
                    static_cast<uint32_t>(frame_bytes * 16), // maxlength: 160 ms
                    static_cast<uint32_t>(frame_bytes * 6),  // tlength: 60 ms
                    default_value,                            // prebuf: default (= tlength)
                    static_cast<uint32_t>(frame_bytes * 2),  // minreq: 20 ms
                    default_value
                };
                if (pa_stream_connect_playback(playback, nullptr, &attributes,
                        static_cast<pa_stream_flags_t>(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_AUTO_TIMING_UPDATE),
                        nullptr, nullptr) < 0) {
                    closeStream(playback);
                }
            }
        }
        if (!microphone) {
            closeStream(recording);
            recorded.clear();
        } else if (!recording) {
            recording = pa_stream_new(context, "Remote microphone", &sample_spec, nullptr);
            if (!recording) return;
            pa_stream_set_read_callback(recording, [](pa_stream* stream, size_t, void* ctx) {
                static_cast<Impl*>(ctx)->record(stream);
            }, this);
            const pa_buffer_attr attributes{static_cast<uint32_t>(frame_bytes * 8), default_value, default_value,
                default_value, static_cast<uint32_t>(frame_bytes)};
            if (pa_stream_connect_record(recording, nullptr, &attributes, PA_STREAM_ADJUST_LATENCY) < 0) {
                closeStream(recording);
            }
        }
    }

    void writePlayback(pa_stream* stream, size_t nbytes) {
        size_t remaining = nbytes - (nbytes % sample_frame_bytes);
        const float vol = gain.load(std::memory_order_relaxed);
        while (remaining >= sample_frame_bytes) {
            void* dst{};
            size_t chunk = remaining;
            if (pa_stream_begin_write(stream, &dst, &chunk) < 0 || !dst || chunk < sample_frame_bytes) break;
            chunk -= chunk % sample_frame_bytes;
            playback_ring.pop(static_cast<std::int16_t*>(dst), chunk / sizeof(std::int16_t), vol);
            if (pa_stream_write(stream, dst, chunk, nullptr, 0, PA_SEEK_RELATIVE) < 0) break;
            remaining -= chunk;
        }
    }

    void record(pa_stream* stream) {
        while (microphone && pa_stream_readable_size(stream) > 0) {
            const void* source{};
            size_t available{};
            if (pa_stream_peek(stream, &source, &available) < 0 || available == 0) break;
            // Skip stale chunks when the audio server has been suspended.
            const auto count = std::min<size_t>(available, static_cast<size_t>(frame_bytes * 4));
            if (source) recorded.append(static_cast<const char*>(source) + available - count, static_cast<qsizetype>(count));
            else recorded.append(QByteArray(static_cast<qsizetype>(count), '\0'));
            pa_stream_drop(stream);
            while (recorded.size() >= frame_bytes) {
                input(recorded.first(frame_bytes));
                recorded.remove(0, frame_bytes);
            }
        }
    }

    Input input;
    QByteArray recorded;
    PcmRingBuffer playback_ring;
    pa_threaded_mainloop* loop{};
    pa_context* context{};
    pa_stream* playback{};
    pa_stream* recording{};
    std::atomic<float> gain{1.0f};
    std::atomic<bool> speaker_enabled{false};
    bool running{}, speaker{}, microphone{};
};

ViewerAudio::ViewerAudio(Input input) : impl_(std::make_unique<Impl>(std::move(input))) {}
ViewerAudio::~ViewerAudio() = default;

void ViewerAudio::configure(bool speaker, bool mic, float volume) {
    impl_->gain.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    impl_->speaker_enabled.store(speaker, std::memory_order_release);
    if (!impl_->running) return;
    const PulseLock lock(impl_->loop);
    impl_->speaker = speaker;
    impl_->microphone = mic;
    impl_->syncStreams();
}

void ViewerAudio::play(const char* pcm, std::size_t size) {
    if (!pcm || size < sample_frame_bytes || (size % sample_frame_bytes) != 0) return;
    if (!impl_->speaker_enabled.load(std::memory_order_acquire)) return;
    impl_->playback_ring.push(reinterpret_cast<const std::int16_t*>(pcm), size / sizeof(std::int16_t));
}
