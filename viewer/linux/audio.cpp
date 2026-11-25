#include "audio.h"

#include <QtEndian>

#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace {

constexpr qsizetype frame_bytes = 960;
constexpr pa_sample_spec sample_spec{PA_SAMPLE_S16LE, 48'000, 1};
constexpr auto default_value = static_cast<uint32_t>(-1);

void closeStream(pa_stream*& stream) {
    if (!stream) return;
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
        if (!speaker) closeStream(playback);
        else if (!playback) {
            playback = pa_stream_new(context, "Remote desktop audio", &sample_spec, nullptr);
            const pa_buffer_attr attributes{frame_bytes * 8, frame_bytes * 4, frame_bytes * 2, frame_bytes, default_value};
            if (playback && pa_stream_connect_playback(playback, nullptr, &attributes, PA_STREAM_ADJUST_LATENCY, nullptr, nullptr) < 0) {
                closeStream(playback);
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
            const pa_buffer_attr attributes{frame_bytes * 8, default_value, default_value, default_value, frame_bytes};
            if (pa_stream_connect_record(recording, nullptr, &attributes, PA_STREAM_ADJUST_LATENCY) < 0) {
                closeStream(recording);
            }
        }
    }

    void record(pa_stream* stream) {
        while (microphone && pa_stream_readable_size(stream) > 0) {
            const void* source{};
            size_t available{};
            if (pa_stream_peek(stream, &source, &available) < 0 || available == 0) break;
            // Skip stale chunks when the audio server has been suspended.
            const auto count = std::min<size_t>(available, frame_bytes * 4);
            if (source) recorded.append(static_cast<const char*>(source) + available - count, count);
            else recorded.append(QByteArray(count, '\0'));
            pa_stream_drop(stream);
            while (recorded.size() >= frame_bytes) {
                input(recorded.first(frame_bytes));
                recorded.remove(0, frame_bytes);
            }
        }
    }

    Input input;
    QByteArray recorded;
    pa_threaded_mainloop* loop{};
    pa_context* context{};
    pa_stream* playback{};
    pa_stream* recording{};
    float gain{1.0f};
    bool running{}, speaker{}, microphone{};
};

ViewerAudio::ViewerAudio(Input input) : impl_(std::make_unique<Impl>(std::move(input))) {}
ViewerAudio::~ViewerAudio() = default;

void ViewerAudio::configure(bool speaker, bool mic, float volume) {
    if (!impl_->running) return;
    const PulseLock lock(impl_->loop);
    impl_->speaker = speaker;
    impl_->microphone = mic;
    impl_->gain = std::clamp(volume, 0.0f, 1.0f);
    impl_->syncStreams();
}

void ViewerAudio::play(QByteArray pcm) {
    if (!impl_->running || pcm.size() % sizeof(qint16)) return;
    const PulseLock lock(impl_->loop);
    const auto stream = impl_->playback;
    if (!stream || pa_stream_get_state(stream) != PA_STREAM_READY) return;
    const auto available = pa_stream_writable_size(stream);
    if (available == static_cast<size_t>(-1) || available < static_cast<size_t>(pcm.size())) return;
    if (impl_->gain < 1.0f) {
        for (qsizetype offset = 0; offset < pcm.size(); offset += sizeof(qint16)) {
            const auto sample = qFromLittleEndian<qint16>(pcm.constData() + offset);
            qToLittleEndian(static_cast<qint16>(sample * impl_->gain), pcm.data() + offset);
        }
    }
    pa_stream_write(stream, pcm.constData(), pcm.size(), nullptr, 0, PA_SEEK_RELATIVE);
}
