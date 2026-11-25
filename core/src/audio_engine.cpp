#include "audio_engine.h"

#include <windows.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <thread>
#include <utility>

namespace p2premote {
namespace {

// PKEY_Device_FriendlyName {a45c254e-df1c-4efd-8020-67d146a850e0}, 14
constexpr PROPERTYKEY kDeviceFriendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
    14
};

WAVEFORMATEX TargetPcmFormat() noexcept {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = audio_channels;
    wfx.nSamplesPerSec = audio_sample_rate;
    wfx.wBitsPerSample = audio_bits_per_sample;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * (wfx.wBitsPerSample / 8));
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    return wfx;
}

class MmcssGuard {
public:
    MmcssGuard() noexcept {
        DWORD task_index{};
        handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    }
    ~MmcssGuard() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }
    MmcssGuard(const MmcssGuard&) = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;

private:
    HANDLE handle_{};
};

struct ComGuard {
    explicit ComGuard(DWORD flags = COINIT_MULTITHREADED) noexcept {
        hr = CoInitializeEx(nullptr, flags);
    }
    ~ComGuard() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    HRESULT hr{E_FAIL};
};

std::wstring ToLowerWide(std::wstring_view text) {
    std::wstring out(text);
    for (auto& ch : out) ch = static_cast<wchar_t>(std::towlower(ch));
    return out;
}

bool LooksLikeVirtualMicSink(std::wstring_view name) {
    const auto lower = ToLowerWide(name);
    return lower.find(L"cable input") != std::wstring::npos
        || lower.find(L"vb-audio") != std::wstring::npos
        || lower.find(L"virtual cable") != std::wstring::npos
        || lower.find(L"voicemeeter input") != std::wstring::npos
        || lower.find(L"virtual audio") != std::wstring::npos;
}

std::wstring DeviceFriendlyName(IMMDevice* device) {
    if (!device) return {};
    winrt::com_ptr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.put()))) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kDeviceFriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

winrt::com_ptr<IMMDevice> FindVirtualMicRenderDevice(IMMDeviceEnumerator* enumerator) {
    if (!enumerator) return {};
    winrt::com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()))) return {};
    UINT count{};
    if (FAILED(collection->GetCount(&count))) return {};
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())) || !device) continue;
        if (LooksLikeVirtualMicSink(DeviceFriendlyName(device.get()))) return device;
    }
    return {};
}

class ProcessLoopbackActivator
    : public winrt::implements<ProcessLoopbackActivator, IActivateAudioInterfaceCompletionHandler> {
public:
    ProcessLoopbackActivator() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) noexcept override {
        if (operation) {
            HRESULT hr_activate = E_FAIL;
            winrt::com_ptr<IUnknown> unknown;
            if (SUCCEEDED(operation->GetActivateResult(&hr_activate, unknown.put())) && SUCCEEDED(hr_activate) && unknown) {
                hr_ = unknown->QueryInterface(__uuidof(IAudioClient), client_.put_void());
            } else {
                hr_ = FAILED(hr_activate) ? hr_activate : E_FAIL;
            }
        }
        if (done_) SetEvent(done_.get());
        return S_OK;
    }

    winrt::com_ptr<IAudioClient> Wait(DWORD timeout_ms = 2'000) {
        if (!done_ || WaitForSingleObject(done_.get(), timeout_ms) != WAIT_OBJECT_0 || FAILED(hr_)) return {};
        return client_;
    }

private:
    winrt::handle done_;
    HRESULT hr_{E_FAIL};
    winrt::com_ptr<IAudioClient> client_;
};

winrt::com_ptr<IAudioClient> TryActivateProcessLoopback() {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activate_params{};
    activate_params.vt = VT_BLOB;
    activate_params.blob.cbSize = sizeof(params);
    activate_params.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    auto handler = winrt::make_self<ProcessLoopbackActivator>();
    winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activate_params,
        handler.get(),
        operation.put());
    if (FAILED(hr) || !operation) return {};
    return handler->Wait();
}

} // namespace

AudioRingBuffer::AudioRingBuffer(std::size_t capacity_samples, std::size_t prebuffer_samples,
    std::size_t trim_target_samples)
    : buffer_(std::max<std::size_t>(capacity_samples - (capacity_samples % audio_channels), audio_frame_samples * 4), 0),
      prebuffer_samples_(std::min(prebuffer_samples - (prebuffer_samples % audio_channels), buffer_.size())),
      trim_target_samples_(std::min(trim_target_samples - (trim_target_samples % audio_channels), buffer_.size())) {}

void AudioRingBuffer::Push(const std::int16_t* samples, std::size_t count) {
    count -= count % audio_channels;
    if (!samples || !count) return;
    const std::lock_guard lock(mutex_);
    if (count >= buffer_.size()) {
        samples += count - trim_target_samples_;
        count = trim_target_samples_;
        read_pos_ = write_pos_ = count_ = 0;
    } else if (count_ + count > buffer_.size()) {
        std::size_t keep = std::min(count_, trim_target_samples_ > count ? trim_target_samples_ - count : 0);
        keep -= keep % audio_channels;
        const std::size_t drop = count_ - keep;
        read_pos_ = (read_pos_ + drop) % buffer_.size();
        count_ = keep;
    }
    for (std::size_t i = 0; i < count; ++i) {
        buffer_[write_pos_] = samples[i];
        write_pos_ = (write_pos_ + 1) % buffer_.size();
    }
    count_ += count;
    if (!primed_ && count_ >= prebuffer_samples_) primed_ = true;
}

std::size_t AudioRingBuffer::Pop(std::int16_t* destination, std::size_t count, float volume) {
    if (!destination || !count) return 0;
    const std::lock_guard lock(mutex_);
    if (!primed_ || count_ < count) {
        std::memset(destination, 0, count * sizeof(std::int16_t));
        if (count_ == 0) primed_ = false;
        return 0;
    }
    const float gain = std::clamp(volume, 0.0f, 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const std::int16_t sample = buffer_[read_pos_];
        read_pos_ = (read_pos_ + 1) % buffer_.size();
        if (gain >= 0.999f) {
            destination[i] = sample;
        } else if (gain <= 0.001f) {
            destination[i] = 0;
        } else {
            const int scaled = static_cast<int>(std::lround(static_cast<float>(sample) * gain));
            destination[i] = static_cast<std::int16_t>(std::clamp(scaled, -32768, 32767));
        }
    }
    count_ -= count;
    if (count_ == 0) primed_ = false;
    return count;
}

void AudioRingBuffer::Clear() {
    const std::lock_guard lock(mutex_);
    read_pos_ = write_pos_ = count_ = 0;
    primed_ = false;
}

std::size_t AudioRingBuffer::Size() const {
    const std::lock_guard lock(mutex_);
    return count_;
}

struct AudioCapture::Impl {
    Impl(Mode mode, FrameCallback on_frame)
        : mode(mode), on_frame(std::move(on_frame)), stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (stop_event) {
            worker = std::jthread([this](std::stop_token token) { Run(token); });
        }
    }

    ~Impl() { Stop(); }

    void Stop() {
        if (worker.joinable()) {
            worker.request_stop();
            if (stop_event) SetEvent(stop_event.get());
            worker.join();
        }
    }

    void Run(std::stop_token token) noexcept {
        ComGuard com(COINIT_MULTITHREADED);
        MmcssGuard mmcss;
        while (!token.stop_requested()) {
            if (!RunOnce(token)) {
                if (token.stop_requested()) break;
                WaitForSingleObject(stop_event.get(), 300);
            }
        }
    }

    bool RunOnce(std::stop_token token) noexcept {
        winrt::com_ptr<IAudioClient> client;
        bool use_event = false;
        const auto wfx = TargetPcmFormat();

        if (mode == Mode::desktop_loopback) {
            client = TryActivateProcessLoopback();
            if (client) {
                const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                if (SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &wfx, nullptr))) {
                    use_event = true;
                } else {
                    client = nullptr;
                }
            }
            if (!client) {
                winrt::com_ptr<IMMDeviceEnumerator> enumerator;
                winrt::com_ptr<IMMDevice> device;
                if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        __uuidof(IMMDeviceEnumerator), enumerator.put_void()))
                    || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))
                    || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
                    return false;
                }
                const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK
                    | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                constexpr REFERENCE_TIME buffer_duration = 400'000; // 40 ms
                if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &wfx, nullptr))) {
                    return false;
                }
                use_event = false;
            }
        } else {
            winrt::com_ptr<IMMDeviceEnumerator> enumerator;
            winrt::com_ptr<IMMDevice> device;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                    __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
                return false;
            }
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, device.put()))
                && FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.put()))) {
                return false;
            }
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
                return false;
            }
            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            constexpr REFERENCE_TIME buffer_duration = 400'000; // 40 ms
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &wfx, nullptr))) {
                return false;
            }
            use_event = true;
        }

        winrt::handle sample_event;
        if (use_event) {
            sample_event.attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
            if (!sample_event || FAILED(client->SetEventHandle(sample_event.get()))) return false;
        }

        winrt::com_ptr<IAudioCaptureClient> capture;
        if (FAILED(client->GetService(__uuidof(IAudioCaptureClient), capture.put_void()))) return false;
        if (FAILED(client->Start())) return false;

        std::vector<char> pending;
        pending.reserve(audio_frame_bytes * 4);
        bool ok = true;

        while (!token.stop_requested()) {
            if (use_event) {
                HANDLE handles[2]{stop_event.get(), sample_event.get()};
                const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, 100);
                if (waited == WAIT_OBJECT_0) break;
            } else {
                if (WaitForSingleObject(stop_event.get(), 10) == WAIT_OBJECT_0) break;
            }

            UINT32 packet_frames{};
            while (SUCCEEDED(capture->GetNextPacketSize(&packet_frames)) && packet_frames > 0) {
                BYTE* data{};
                UINT32 frames{};
                DWORD flags{};
                const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                    ok = false;
                    break;
                }
                if (FAILED(hr)) break;

                const std::size_t bytes = static_cast<std::size_t>(frames) * wfx.nBlockAlign;
                const bool is_paused = paused.load(std::memory_order_acquire);
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (!is_paused && !silent && data && bytes > 0) {
                    const auto* raw = reinterpret_cast<const char*>(data);
                    pending.insert(pending.end(), raw, raw + bytes);
                } else if (is_paused) {
                    pending.clear();
                }
                capture->ReleaseBuffer(frames);

                while (pending.size() >= audio_frame_bytes) {
                    if (on_frame && !paused.load(std::memory_order_acquire)) {
                        try { on_frame(pending.data(), audio_frame_bytes); } catch (...) {}
                    }
                    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(audio_frame_bytes));
                }
            }
            if (!ok) break;
        }

        client->Stop();
        return ok;
    }

    Mode mode;
    FrameCallback on_frame;
    std::atomic_bool paused{false};
    winrt::handle stop_event;
    std::jthread worker;
};

AudioCapture::AudioCapture(Mode mode, FrameCallback on_frame)
    : impl_(std::make_unique<Impl>(mode, std::move(on_frame))) {}
AudioCapture::~AudioCapture() = default;
void AudioCapture::SetPaused(bool paused) noexcept { impl_->paused.store(paused, std::memory_order_release); }
void AudioCapture::Stop() { impl_->Stop(); }

struct AudioPlayer::Impl {
    Impl(Target target, bool allow_speaker_fallback)
        : target(target),
          allow_speaker_fallback(allow_speaker_fallback),
          stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (stop_event) {
            worker = std::jthread([this](std::stop_token token) { Run(token); });
        }
    }

    ~Impl() { Stop(); }

    void Stop() {
        if (worker.joinable()) {
            worker.request_stop();
            if (stop_event) SetEvent(stop_event.get());
            worker.join();
        }
    }

    void Run(std::stop_token token) noexcept {
        ComGuard com(COINIT_MULTITHREADED);
        MmcssGuard mmcss;
        while (!token.stop_requested()) {
            if (!RunOnce(token)) {
                if (token.stop_requested()) break;
                WaitForSingleObject(stop_event.get(), 400);
            }
        }
    }

    bool RunOnce(std::stop_token token) noexcept {
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
            return false;
        }

        winrt::com_ptr<IMMDevice> device;
        bool is_virtual = false;
        if (target == Target::virtual_mic_or_speaker) {
            device = FindVirtualMicRenderDevice(enumerator.get());
            if (device) {
                is_virtual = true;
            } else if (allow_speaker_fallback) {
                enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
            }
        } else {
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
        }
        using_virtual_mic.store(is_virtual, std::memory_order_release);
        if (!device) return false;

        winrt::com_ptr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) return false;

        const auto wfx = TargetPcmFormat();
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        constexpr REFERENCE_TIME buffer_duration = 300'000; // 30 ms
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer_duration, 0, &wfx, nullptr))) {
            return false;
        }

        UINT32 buffer_frames{};
        if (FAILED(client->GetBufferSize(&buffer_frames)) || !buffer_frames) return false;

        winrt::handle render_event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!render_event || FAILED(client->SetEventHandle(render_event.get()))) return false;

        winrt::com_ptr<IAudioRenderClient> render;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), render.put_void()))) return false;

        // Pre-fill silence before starting playback.
        BYTE* initial{};
        if (SUCCEEDED(render->GetBuffer(buffer_frames, &initial)) && initial) {
            std::memset(initial, 0, static_cast<std::size_t>(buffer_frames) * wfx.nBlockAlign);
            render->ReleaseBuffer(buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        if (FAILED(client->Start())) return false;

        bool ok = true;
        while (!token.stop_requested()) {
            HANDLE handles[2]{stop_event.get(), render_event.get()};
            const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, 100);
            if (waited == WAIT_OBJECT_0) break;

            UINT32 padding{};
            HRESULT hr = client->GetCurrentPadding(&padding);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                ok = false;
                break;
            }
            if (FAILED(hr) || padding >= buffer_frames) continue;

            const UINT32 available = buffer_frames - padding;
            BYTE* out{};
            hr = render->GetBuffer(available, &out);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
                ok = false;
                break;
            }
            if (FAILED(hr) || !out) continue;

            const bool is_muted = muted.load(std::memory_order_acquire);
            const float vol = is_muted ? 0.0f : volume.load(std::memory_order_acquire);
            const auto popped = ring.Pop(reinterpret_cast<std::int16_t*>(out),
                static_cast<std::size_t>(available) * audio_channels, vol);
            const DWORD release_flags = (is_muted || popped == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
            render->ReleaseBuffer(available, release_flags);
        }

        client->Stop();
        return ok;
    }

    Target target;
    bool allow_speaker_fallback;
    AudioRingBuffer ring;
    std::atomic_bool muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic_bool using_virtual_mic{false};
    winrt::handle stop_event;
    std::jthread worker;
};

AudioPlayer::AudioPlayer(Target target, bool allow_speaker_fallback)
    : impl_(std::make_unique<Impl>(target, allow_speaker_fallback)) {}
AudioPlayer::~AudioPlayer() = default;

void AudioPlayer::PushPcm(const char* data, std::size_t size) {
    const std::size_t stride = audio_channels * sizeof(std::int16_t);
    size -= size % stride;
    if (!data || size < stride) return;
    impl_->ring.Push(reinterpret_cast<const std::int16_t*>(data), size / sizeof(std::int16_t));
}

void AudioPlayer::SetMuted(bool muted) noexcept {
    impl_->muted.store(muted, std::memory_order_release);
    if (muted) impl_->ring.Clear();
}

void AudioPlayer::SetVolume(float volume) noexcept {
    impl_->volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

bool AudioPlayer::UsingVirtualMic() const noexcept {
    return impl_->using_virtual_mic.load(std::memory_order_acquire);
}

void AudioPlayer::Stop() { impl_->Stop(); }

} // namespace p2premote
