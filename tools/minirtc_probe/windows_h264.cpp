#include "windows_h264.h"
#include "dxgi_nv12_frame.h"

#include "openh264_encoder.h"
#include "openh264_decoder.h"

#include <codecapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <deque>
#include <mutex>

namespace minirtc {
namespace {

using winrt::check_hresult;
using winrt::com_ptr;
using p2premote::native_minirtc::DxgiNv12Frame;

struct Apartment {
    Apartment() : initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~Apartment() { if (initialized) CoUninitialize(); }
    bool initialized;
};

bool StartMediaFoundation() {
    struct Runtime {
        bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        ~Runtime() { if (started) MFShutdown(); }
    };
    static const Runtime runtime;
    return runtime.started;
}

com_ptr<IMFActivate> EncoderActivation(ID3D11Device* device) {
    com_ptr<IDXGIAdapter> adapter;
    com_ptr<IDXGIDevice> dxgi;
    check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));
    check_hresult(dxgi->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC desc{};
    check_hresult(adapter->GetDesc(&desc));
    com_ptr<IMFAttributes> attributes;
    check_hresult(MFCreateAttributes(attributes.put(), 1));
    check_hresult(attributes->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const BYTE*>(&desc.AdapterLuid), sizeof(LUID)));
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** entries{};
    UINT32 count{};
    check_hresult(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, attributes.get(), &entries, &count));
    com_ptr<IMFActivate> activation;
    if (count) activation.copy_from(entries[0]);
    for (UINT32 i = 0; i < count; ++i) entries[i]->Release();
    CoTaskMemFree(entries);
    return activation;
}

com_ptr<IMFMediaType> VideoType(const GUID& subtype, UINT width = 0, UINT height = 0, UINT fps = 0) {
    com_ptr<IMFMediaType> type;
    check_hresult(MFCreateMediaType(type.put()));
    check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype));
    check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    if (width && height) check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height));
    if (fps) check_hresult(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, fps, 1));
    return type;
}

bool SetNumber(ICodecAPI* api, const GUID& key, ULONG number) {
    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = number;
    return api && SUCCEEDED(api->SetValue(&key, &value));
}

void Enable(ICodecAPI* api, const GUID& key) {
    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    if (api) api->SetValue(&key, &value);
}

struct Transform {
    Transform(com_ptr<IMFTransform> codec, ID3D11Device* device) : mft(std::move(codec)) {
        UINT token{};
        check_hresult(MFCreateDXGIDeviceManager(&token, manager.put()));
        check_hresult(manager->ResetDevice(device, token));
        check_hresult(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.get())));
        if (const auto result = mft->GetStreamIDs(1, &input, 1, &output); result != E_NOTIMPL) check_hresult(result);
    }

    ~Transform() {
        if (auto shutdown = mft.try_as<IMFShutdown>()) shutdown->Shutdown();
        else mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    void Start() {
        check_hresult(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        check_hresult(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    HRESULT Pull(com_ptr<IMFSample>& sample) {
        MFT_OUTPUT_STREAM_INFO info{};
        check_hresult(mft->GetOutputStreamInfo(output, &info));
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            com_ptr<IMFMediaBuffer> buffer;
            check_hresult(MFCreateSample(sample.put()));
            check_hresult(MFCreateAlignedMemoryBuffer(info.cbSize, info.cbAlignment ? info.cbAlignment - 1 : 0, buffer.put()));
            check_hresult(sample->AddBuffer(buffer.get()));
        }
        MFT_OUTPUT_DATA_BUFFER entry{output, sample.get(), 0, nullptr};
        DWORD status{};
        const HRESULT result = mft->ProcessOutput(0, 1, &entry, &status);
        if (entry.pEvents) entry.pEvents->Release();
        if (entry.pSample != sample.get()) sample.attach(entry.pSample);
        return result;
    }

    com_ptr<IMFTransform> mft;
    com_ptr<IMFDXGIDeviceManager> manager;
    DWORD input{}, output{};
};

class EventWaiter : public winrt::implements<EventWaiter, IMFAsyncCallback> {
public:
    HRESULT __stdcall GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall Invoke(IMFAsyncResult* result) noexcept override {
        const std::lock_guard lock(mutex_);
        if (cancelled_) return S_OK;
        result_.copy_from(result);
        SetEvent(ready_.get());
        return S_OK;
    }

    MediaEventType Next(IMFMediaEventGenerator* generator, ULONGLONG deadline) {
        com_ptr<IMFMediaEvent> event;
        auto status = generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.put());
        if (status == MF_E_NO_EVENTS_AVAILABLE) {
            const auto now = GetTickCount64();
            if (now >= deadline) winrt::throw_hresult(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            winrt::check_pointer(ready_.get());
            check_hresult(generator->BeginGetEvent(this, nullptr));
            if (WaitForSingleObject(ready_.get(), static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
                winrt::throw_hresult(HRESULT_FROM_WIN32(WAIT_TIMEOUT));
            com_ptr<IMFAsyncResult> result;
            {
                const std::lock_guard lock(mutex_);
                result = std::move(result_);
            }
            status = generator->EndGetEvent(result.get(), event.put());
        }
        check_hresult(status);
        check_hresult(event->GetStatus(&status));
        check_hresult(status);
        MediaEventType type{};
        check_hresult(event->GetType(&type));
        return type;
    }

    void Cancel() {
        const std::lock_guard lock(mutex_);
        cancelled_ = true;
        result_ = nullptr;
    }

private:
    std::mutex mutex_;
    winrt::handle ready_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    com_ptr<IMFAsyncResult> result_;
    bool cancelled_{};
};

struct BufferLock {
    explicit BufferLock(IMFMediaBuffer* buffer) : buffer(buffer) {
        check_hresult(buffer->Lock(&data, nullptr, &size));
    }
    ~BufferLock() { buffer->Unlock(); }
    IMFMediaBuffer* buffer;
    BYTE* data{};
    DWORD size{};
};

unsigned AnnexBUnits(const uint8_t* data, size_t size) {
    unsigned mask{};
    for (size_t i = 2; i + 1 < size; ++i) {
        if (data[i - 2] == 0 && data[i - 1] == 0 && data[i] == 1) mask |= 1u << (data[i + 1] & 31);
    }
    return mask;
}

class WindowsEncoder final : public MediaCodec {
public:
    WindowsEncoder(std::shared_ptr<SystemClock> clock, ID3D11Device* device) : clock_(std::move(clock)) { device_.copy_from(device); }
    ~WindowsEncoder() override { Apartment apartment; CloseHardware(); }

    int Init(const MediaCodecConfig& config) override {
        Apartment apartment;
        config_ = config;
        try {
            if (!StartMediaFoundation()) winrt::throw_hresult(E_FAIL);
            Open(config.init_width, config.init_height);
            return 0;
        } catch (const winrt::hresult_error& error) {
            LOG_WARN("Hardware H.264 encoder initialization failed (0x{:08x})", static_cast<uint32_t>(error.code().value));
            return StartSoftware();
        }
    }

    std::string GetEncoderName() const override { return software_ ? software_->GetEncoderName() : "Media Foundation H.264 (hardware)"; }
    int GetResolution(int* width, int* height) const override {
        const auto dimensions = resolution_.load(std::memory_order_relaxed);
        if (!width || !height || !dimensions) return -1;
        *width = static_cast<int>(dimensions >> 32);
        *height = static_cast<int>(static_cast<uint32_t>(dimensions));
        return 0;
    }
    bool SupportsNativeFrameInput(MiniRtcNativeVideoFrameType type) const override {
        return !software_ && type == MiniRtcNativeVideoFrameD3d11Nv12;
    }
    int ForceIdr() override {
        if (software_) return software_->ForceIdr();
        force_idr_ = true;
        return 0;
    }
    int SetTargetBitrate(int bitrate) override {
        Apartment apartment;
        if (bitrate <= 0) return -1;
        if (software_) return software_->SetTargetBitrate(bitrate);
        if (bitrate == config_.init_bitrate) return 0;
        if (!SetNumber(api_.get(), CODECAPI_AVEncCommonMeanBitRate, bitrate)) return -1;
        SetDesktopBufferSize(api_.get(), bitrate);
        // A static desktop can benefit from a fresh high-quality reference.
        // During motion, let the regular GOP handle it instead of adding an IDR
        // just as the bandwidth estimate starts growing.
        if (config_.init_bitrate > 0 && bitrate >= 2LL * config_.init_bitrate) {
            if (config_.video_content_type == VideoContentType::ScreenContent) refresh_static_ = true;
            else force_idr_ = true;
        }
        config_.init_bitrate = bitrate;
        return 0;
    }

    int Encode(const RawFrame& raw, std::function<int(const EncodedFrame&)> output) override {
        Apartment apartment;
        if (!software_) {
            try {
                EncodeHardware(raw, output);
                return 0;
            } catch (const winrt::hresult_error& error) {
                LOG_WARN("Hardware H.264 encode failed (0x{:08x})", static_cast<uint32_t>(error.code().value));
                if (StartSoftware() != 0) return -1;
            }
        }
        resolution_.store((static_cast<uint64_t>(raw.Width()) << 32) | raw.Height(), std::memory_order_relaxed);
        if (!raw.NativeFrame()) return software_->Encode(raw, std::move(output));
        RawFrame readable(raw);
        return readable.MaterializeNativeFrame() ? software_->Encode(readable, std::move(output)) : -1;
    }

private:
    void SetDesktopBufferSize(ICodecAPI* api, int bitrate) const {
        if (config_.video_content_type != VideoContentType::ScreenContent || bitrate <= 0) return;
        // H.264 HRD capacity is in bytes. A 125 ms buffer retains text detail
        // across scroll frames while bounding the credit accumulated at idle.
        constexpr int buffers_per_second = 8;
        SetNumber(api, CODECAPI_AVEncCommonBufferSize, std::max(1, bitrate / 8 / buffers_per_second));
    }

    void CloseHardware() {
        if (waiter_) waiter_->Cancel();
        generator_ = nullptr;
        api_ = nullptr;
        transform_.reset();
        waiter_ = nullptr;
    }

    int StartSoftware() {
        CloseHardware();
        software_ = std::make_unique<OpenH264Encoder>(clock_);
        LOG_INFO("Using OpenH264 software encoder fallback");
        return software_->Init(config_);
    }

    void Open(UINT width, UINT height) {
        CloseHardware();
        try {
            OpenProfile(width, height, eAVEncH264VProfile_Main);
        } catch (const winrt::hresult_error&) {
            // Retry on a fresh MFT if any part of Main profile setup failed.
            OpenProfile(width, height, eAVEncH264VProfile_Base);
        }
        width_ = width;
        height_ = height;
        resolution_.store((static_cast<uint64_t>(width) << 32) | height, std::memory_order_relaxed);
        input_requests_ = 0;
    }

    void OpenProfile(UINT width, UINT height, UINT32 profile) {
        const auto activate = EncoderActivation(device_.get());
        if (!activate) winrt::throw_hresult(MF_E_TOPO_CODEC_NOT_FOUND);
        com_ptr<IMFTransform> mft;
        check_hresult(activate->ActivateObject(__uuidof(IMFTransform), mft.put_void()));
        activate->DetachObject();
        com_ptr<IMFAttributes> attributes;
        check_hresult(mft->GetAttributes(attributes.put()));
        check_hresult(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        auto transform = std::make_unique<Transform>(std::move(mft), device_.get());
        auto generator = transform->mft.as<IMFMediaEventGenerator>();
        auto api = transform->mft.try_as<ICodecAPI>();
        Enable(api.get(), CODECAPI_AVLowLatencyMode);
        SetNumber(api.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        SetNumber(api.get(), CODECAPI_AVEncCommonQualityVsSpeed, 70);
        SetNumber(api.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        const auto fps = static_cast<UINT>(std::clamp(config_.max_frame_rate, 1, 60));
        // Use a long GOP (matching MiniRTC's other encoders) and rely on on-demand
        // PLI/FIR ForceIdr() for loss/join recovery to avoid periodic IDR stutters.
        const auto gop = static_cast<UINT>(config_.key_frame_interval > 0 ? config_.key_frame_interval : 3000);
        SetNumber(api.get(), CODECAPI_AVEncMPVGOPSize, gop);
        if (config_.video_content_type == VideoContentType::ScreenContent && config_.init_bitrate > 0) {
            SetDesktopBufferSize(api.get(), config_.init_bitrate);
            SetNumber(api.get(), CODECAPI_AVEncCommonMeanBitRate, config_.init_bitrate);
        }
        const auto h264 = VideoType(MFVideoFormat_H264, width, height, fps);
        check_hresult(h264->SetUINT32(MF_MT_AVG_BITRATE, config_.init_bitrate));
        check_hresult(h264->SetUINT32(MF_MT_MPEG2_PROFILE, profile));
        check_hresult(transform->mft->SetOutputType(transform->output, h264.get(), 0));
        const auto nv12 = VideoType(MFVideoFormat_NV12, width, height, fps);
        check_hresult(transform->mft->SetInputType(transform->input, nv12.get(), 0));
        transform->Start();
        waiter_ = winrt::make_self<EventWaiter>();
        api_ = std::move(api);
        generator_ = std::move(generator);
        transform_ = std::move(transform);
    }

    void EncodeHardware(const RawFrame& raw, const std::function<int(const EncodedFrame&)>& output) {
        if (raw.Width() != width_ || raw.Height() != height_) Open(raw.Width(), raw.Height());
        const auto captured = raw.CapturedTimestamp();
        const bool sparse = last_capture_us_ > 0 && captured - last_capture_us_ >= 250'000;
        if (refresh_static_ && sparse && previous_sparse_) force_idr_ = true;
        last_capture_us_ = captured;
        previous_sparse_ = sparse;
        if (std::exchange(force_idr_, false) && !SetNumber(api_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1)) Open(width_, height_);
        const auto* native = raw.NativeFrame();
        com_ptr<ID3D11Texture2D> uploaded;
        ID3D11Texture2D* texture{};
        UINT subresource{};
        if (native && native->type == MiniRtcNativeVideoFrameD3d11Nv12) {
            texture = static_cast<ID3D11Texture2D*>(native->payload.d3d11_nv12.texture);
            subresource = native->payload.d3d11_nv12.subresource;
        } else {
            if (!raw.Buffer()) winrt::throw_hresult(E_INVALIDARG);
            const D3D11_TEXTURE2D_DESC desc{width_, height_, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
            const D3D11_SUBRESOURCE_DATA pixels{raw.Buffer(), width_, static_cast<UINT>(raw.Size())};
            check_hresult(device_->CreateTexture2D(&desc, &pixels, uploaded.put()));
            texture = uploaded.get();
        }
        com_ptr<IMFMediaBuffer> surface;
        com_ptr<IMFSample> input;
        check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, subresource, FALSE, surface.put()));
        check_hresult(MFCreateSample(input.put()));
        check_hresult(input->AddBuffer(surface.get()));
        check_hresult(input->SetSampleTime(raw.CapturedTimestamp() * 10));
        check_hresult(input->SetSampleDuration(10'000'000 / std::clamp(config_.max_frame_rate, 1, 60)));
        const auto deadline = GetTickCount64() + 120;
        while (!input_requests_) if (waiter_->Next(generator_.get(), deadline) == METransformNeedInput) ++input_requests_;
        check_hresult(transform_->mft->ProcessInput(transform_->input, input.get(), 0));
        --input_requests_;
        for (;;) {
            const auto event = waiter_->Next(generator_.get(), deadline);
            if (event == METransformNeedInput) ++input_requests_;
            if (event == METransformHaveOutput) break;
        }
        com_ptr<IMFSample> sample;
        check_hresult(transform_->Pull(sample));
        winrt::check_pointer(sample.get());
        com_ptr<IMFMediaBuffer> bytes;
        check_hresult(sample->ConvertToContiguousBuffer(bytes.put()));
        const BufferLock payload(bytes.get());
        const auto nal = AnnexBUnits(payload.data, payload.size);
        if (!nal) winrt::throw_hresult(MF_E_INVALID_STREAM_DATA);
        constexpr unsigned idr = 1u << 5;
        if (nal & idr) refresh_static_ = false;
        constexpr unsigned parameter_sets = (1u << 7) | (1u << 8);
        std::vector<uint8_t> with_headers;
        if ((nal & idr) && (nal & parameter_sets) != parameter_sets) {
            com_ptr<IMFMediaType> type;
            check_hresult(transform_->mft->GetOutputCurrentType(transform_->output, type.put()));
            UINT32 length{};
            check_hresult(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &length));
            with_headers.resize(length);
            check_hresult(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, with_headers.data(), length, nullptr));
            if ((AnnexBUnits(with_headers.data(), with_headers.size()) & parameter_sets) != parameter_sets)
                winrt::throw_hresult(MF_E_INVALID_STREAM_DATA);
            with_headers.insert(with_headers.end(), payload.data, payload.data + payload.size);
        }
        EncodedFrame encoded(with_headers.empty() ? payload.data : with_headers.data(),
            with_headers.empty() ? payload.size : with_headers.size(), width_, height_);
        encoded.SetEncodedWidth(width_);
        encoded.SetEncodedHeight(height_);
        encoded.SetCapturedTimestamp(raw.CapturedTimestamp());
        encoded.SetEncodedTimestamp(clock_->CurrentTimeUs());
        encoded.SetFrameType(nal & idr ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta);
        if (!reported_) {
            LOG_INFO("Hardware H.264 encoder produced a frame from {} input", uploaded ? "uploaded NV12" : "D3D11 NV12");
            reported_ = true;
        }
        output(encoded);
    }

    std::shared_ptr<SystemClock> clock_;
    com_ptr<ID3D11Device> device_;
    MediaCodecConfig config_;
    std::unique_ptr<Transform> transform_;
    com_ptr<ICodecAPI> api_;
    com_ptr<IMFMediaEventGenerator> generator_;
    com_ptr<EventWaiter> waiter_;
    std::unique_ptr<OpenH264Encoder> software_;
    std::atomic<uint64_t> resolution_{};
    UINT width_{}, height_{}, input_requests_{};
    int64_t last_capture_us_{};
    bool force_idr_{}, refresh_static_{}, previous_sparse_{}, reported_{};
};

class WindowsDecoder final : public MediaCodec {
public:
    WindowsDecoder(std::shared_ptr<SystemClock> clock, ID3D11Device* device) : clock_(std::move(clock)) { device_.copy_from(device); }
    ~WindowsDecoder() override { Apartment apartment; transform_.reset(); }

    int Init() override {
        Apartment apartment;
        try {
            if (!StartMediaFoundation()) winrt::throw_hresult(E_FAIL);
            com_ptr<ID3D11VideoDevice> video = device_.as<ID3D11VideoDevice>();
            BOOL supported{};
            check_hresult(video->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &supported));
            if (!supported) winrt::throw_hresult(MF_E_UNSUPPORTED_D3D_TYPE);
            com_ptr<IMFTransform> mft;
            check_hresult(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IMFTransform), mft.put_void()));
            com_ptr<IMFAttributes> attributes;
            check_hresult(mft->GetAttributes(attributes.put()));
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
            const auto api = mft.try_as<ICodecAPI>();
            Enable(api.get(), CODECAPI_AVLowLatencyMode);
            Enable(api.get(), CODECAPI_AVDecVideoAcceleration_H264);
            transform_ = std::make_unique<Transform>(std::move(mft), device_.get());
            const auto input = VideoType(MFVideoFormat_H264);
            check_hresult(transform_->mft->SetInputType(transform_->input, input.get(), 0));
            ChooseOutput();
            transform_->Start();
            return 0;
        } catch (const winrt::hresult_error& error) {
            LOG_WARN("DXVA H.264 decoder initialization failed (0x{:08x})", static_cast<uint32_t>(error.code().value));
            return StartSoftware();
        }
    }

    std::string GetDecoderName() const override { return software_ ? software_->GetDecoderName() : "Media Foundation H.264 (DXVA requested)"; }

    int Decode(std::unique_ptr<ReceivedFrame> received, std::function<void(const DecodedFrame*)> output) override {
        if (software_) return software_->Decode(std::move(received), std::move(output));
        if (!received || !received->Size() || received->Size() > 8 * 1024 * 1024) return -1;
        Apartment apartment;
        try {
            const int frames = DecodeHardware(*received, output);
            if (frames) failures_ = 0;
            return frames;
        } catch (const winrt::hresult_error& error) {
            LOG_WARN("DXVA H.264 decode failed (0x{:08x})", static_cast<uint32_t>(error.code().value));
            if (++failures_ >= 3 || error.code() == MF_E_UNSUPPORTED_D3D_TYPE) {
                if (StartSoftware() != 0) return -1;
                return software_->Decode(std::move(received), std::move(output));
            }
            timestamps_.clear();
            transform_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            return -1;
        }
    }

private:
    int StartSoftware() {
        timestamps_.clear();
        transform_.reset();
        software_ = std::make_unique<OpenH264Decoder>(clock_, true);
        LOG_INFO("Using OpenH264 software decoder fallback");
        return software_->Init();
    }

    void ChooseOutput() {
        for (DWORD i = 0;; ++i) {
            com_ptr<IMFMediaType> type;
            check_hresult(transform_->mft->GetOutputAvailableType(transform_->output, i, type.put()));
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
            if (FAILED(transform_->mft->SetOutputType(transform_->output, type.get(), 0))) continue;
            check_hresult(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &width_, &height_));
            MFVideoArea aperture{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<BYTE*>(&aperture), sizeof(aperture), nullptr)) &&
                !aperture.OffsetX.value && !aperture.OffsetY.value && aperture.Area.cx > 0 && aperture.Area.cy > 0 &&
                static_cast<UINT>(aperture.Area.cx) <= width_ && static_cast<UINT>(aperture.Area.cy) <= height_) {
                width_ = static_cast<UINT>(aperture.Area.cx);
                height_ = static_cast<UINT>(aperture.Area.cy);
            }
            return;
        }
    }

    int DecodeHardware(const ReceivedFrame& frame, const std::function<void(const DecodedFrame*)>& output) {
        const bool prefix = AnnexBUnits(frame.Buffer(), frame.Size()) == 0;
        const DWORD size = static_cast<DWORD>(frame.Size() + (prefix ? 4 : 0));
        com_ptr<IMFSample> input;
        com_ptr<IMFMediaBuffer> bytes;
        check_hresult(MFCreateSample(input.put()));
        check_hresult(MFCreateMemoryBuffer(size, bytes.put()));
        {
            const BufferLock payload(bytes.get());
            if (prefix) std::memcpy(payload.data, "\0\0\0\1", 4);
            std::memcpy(payload.data + (prefix ? 4 : 0), frame.Buffer(), frame.Size());
        }
        check_hresult(bytes->SetCurrentLength(size));
        check_hresult(input->AddBuffer(bytes.get()));
        check_hresult(input->SetSampleTime(next_time_));
        check_hresult(input->SetSampleDuration(166'667));
        if (timestamps_.size() >= 16) winrt::throw_hresult(MF_E_INVALID_STREAM_DATA);
        check_hresult(transform_->mft->ProcessInput(transform_->input, input.get(), 0));
        timestamps_.push_back({next_time_, frame.CapturedTimestamp(), frame.ReceivedTimestamp()});
        next_time_ += 166'667;
        int frames = 0;
        for (;;) {
            com_ptr<IMFSample> sample;
            const auto result = transform_->Pull(sample);
            if (result == MF_E_TRANSFORM_NEED_MORE_INPUT) return frames;
            if (result == MF_E_TRANSFORM_STREAM_CHANGE) { ChooseOutput(); continue; }
            check_hresult(result);
            winrt::check_pointer(sample.get());
            Emit(std::move(sample), output);
            ++frames;
        }
    }

    void Emit(com_ptr<IMFSample> sample, const std::function<void(const DecodedFrame*)>& output) {
        if (timestamps_.empty()) return;
        LONGLONG time{};
        if (SUCCEEDED(sample->GetSampleTime(&time))) {
            while (timestamps_.size() > 1 && timestamps_.front().time < time) timestamps_.pop_front();
        }
        const auto timestamps = timestamps_.front();
        timestamps_.pop_front();
        com_ptr<IMFMediaBuffer> buffer;
        check_hresult(sample->GetBufferByIndex(0, buffer.put()));
        auto surface = buffer.try_as<IMFDXGIBuffer>();
        if (!surface) winrt::throw_hresult(MF_E_UNSUPPORTED_D3D_TYPE);
        com_ptr<ID3D11Texture2D> texture;
        UINT subresource{};
        check_hresult(surface->GetResource(__uuidof(ID3D11Texture2D), texture.put_void()));
        check_hresult(surface->GetSubresourceIndex(&subresource));
        const auto native = std::unique_ptr<DxgiNv12Frame, decltype(&DxgiNv12Frame::Release)>(
            new DxgiNv12Frame(texture.get(), subresource, width_, height_, sample.as<IUnknown>()), &DxgiNv12Frame::Release);
        DecodedFrame decoded;
        decoded.SetSize(static_cast<size_t>(width_) * height_ * 3 / 2);
        decoded.SetWidth(width_);
        decoded.SetHeight(height_);
        decoded.SetDecodedWidth(width_);
        decoded.SetDecodedHeight(height_);
        decoded.SetCapturedTimestamp(timestamps.captured);
        decoded.SetReceivedTimestamp(timestamps.received);
        decoded.SetDecodedTimestamp(clock_->CurrentTimeUs());
        decoded.SetNativeFrame(&native->descriptor);
        if (!reported_) {
            LOG_INFO("Hardware H.264 decoder produced a D3D11 NV12 frame (DXVA)");
            reported_ = true;
        }
        output(&decoded);
    }

    struct Timestamps { LONGLONG time; int64_t captured; int64_t received; };
    std::shared_ptr<SystemClock> clock_;
    com_ptr<ID3D11Device> device_;
    std::unique_ptr<Transform> transform_;
    std::unique_ptr<OpenH264Decoder> software_;
    std::deque<Timestamps> timestamps_;
    LONGLONG next_time_{};
    UINT width_{}, height_{}, failures_{};
    bool reported_{};
};

} // namespace

std::unique_ptr<MediaCodec> CreateWindowsH264Encoder(std::shared_ptr<SystemClock> clock, void* device) {
    return std::make_unique<WindowsEncoder>(std::move(clock), static_cast<ID3D11Device*>(device));
}

std::unique_ptr<MediaCodec> CreateWindowsH264Decoder(std::shared_ptr<SystemClock> clock, void* device) {
    return std::make_unique<WindowsDecoder>(std::move(clock), static_cast<ID3D11Device*>(device));
}

} // namespace minirtc
