#include "mf_h264.h"

#include "d3d11_video.h"
#include "webrtc_vp8.h"

#include "api/make_ref_counted.h"
#include "api/video/i420_buffer.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_common.h"
#include "libyuv/convert.h"
#include "libyuv/convert_from.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

#include <codecapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

namespace p2premote {
namespace {

constexpr UINT width_limit = 2560;
constexpr UINT height_limit = 1440;

const webrtc::SdpVideoFormat& H264Format() {
    static const webrtc::SdpVideoFormat format("H264", {
        {"profile-level-id", "420033"}, {"level-asymmetry-allowed", "1"}, {"packetization-mode", "1"}});
    return format;
}

class Apartment {
public:
    Apartment() : initialized_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
    ~Apartment() { if (initialized_) CoUninitialize(); }
private:
    bool initialized_;
};

bool MediaFoundationAvailable() {
    struct Platform {
        HMODULE module = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        bool running = module && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        ~Platform() {
            if (running) MFShutdown();
            if (module) FreeLibrary(module);
        }
    };
    static const Platform platform;
    return platform.running;
}

winrt::com_ptr<IMFActivate> EncoderActivation(ID3D11Device* device) {
    winrt::com_ptr<IDXGIAdapter> adapter;
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()));
    winrt::check_hresult(dxgi->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC adapter_desc{};
    winrt::check_hresult(adapter->GetDesc(&adapter_desc));
    winrt::com_ptr<IMFAttributes> attributes;
    winrt::check_hresult(MFCreateAttributes(attributes.put(), 1));
    winrt::check_hresult(attributes->SetBlob(MFT_ENUM_ADAPTER_LUID,
        reinterpret_cast<const BYTE*>(&adapter_desc.AdapterLuid), sizeof(LUID)));
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** entries{};
    UINT32 count{};
    const auto hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input, &output, attributes.get(), &entries, &count);
    winrt::com_ptr<IMFActivate> activation;
    if (SUCCEEDED(hr) && count) activation.copy_from(entries[0]);
    for (UINT32 i = 0; i < count; ++i) entries[i]->Release();
    CoTaskMemFree(entries);
    return activation;
}

bool DecoderAvailable(ID3D11Device* device) {
    winrt::com_ptr<ID3D11VideoDevice> video;
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), video.put_void()))) return false;
    BOOL nv12{};
    return SUCCEEDED(video->CheckVideoDecoderFormat(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, DXGI_FORMAT_NV12, &nv12)) && nv12;
}

winrt::com_ptr<IMFMediaType> MediaType(const GUID& subtype, UINT width = 0, UINT height = 0, UINT fps = 0) {
    winrt::com_ptr<IMFMediaType> type;
    winrt::check_hresult(MFCreateMediaType(type.put()));
    winrt::check_hresult(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    winrt::check_hresult(type->SetGUID(MF_MT_SUBTYPE, subtype));
    winrt::check_hresult(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    if (width && height) winrt::check_hresult(MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, width, height));
    if (fps) winrt::check_hresult(MFSetAttributeRatio(type.get(), MF_MT_FRAME_RATE, fps, 1));
    winrt::check_hresult(MFSetAttributeRatio(type.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    return type;
}

bool SetNumber(ICodecAPI* api, const GUID& key, ULONG number) {
    if (!api) return false;
    VARIANT value{};
    value.vt = VT_UI4;
    value.ulVal = number;
    return SUCCEEDED(api->SetValue(&key, &value));
}

void Enable(ICodecAPI* api, const GUID& key) {
    if (!api) return;
    VARIANT value{};
    value.vt = VT_BOOL;
    value.boolVal = VARIANT_TRUE;
    api->SetValue(&key, &value);
}

class Transform {
public:
    explicit Transform(winrt::com_ptr<IMFTransform> transform, ID3D11Device* device) : mft(std::move(transform)) {
        UINT token{};
        winrt::check_hresult(MFCreateDXGIDeviceManager(&token, manager_.put()));
        winrt::check_hresult(manager_->ResetDevice(device, token));
        winrt::check_hresult(mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager_.get())));
        const auto ids = mft->GetStreamIDs(1, &input, 1, &output);
        if (ids != E_NOTIMPL) winrt::check_hresult(ids);
    }

    ~Transform() {
        if (auto shutdown = mft.try_as<IMFShutdown>()) shutdown->Shutdown();
        else mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    void Start() {
        winrt::check_hresult(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
        winrt::check_hresult(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    }

    HRESULT Pull(winrt::com_ptr<IMFSample>& sample) {
        MFT_OUTPUT_STREAM_INFO info{};
        auto hr = mft->GetOutputStreamInfo(output, &info);
        if (FAILED(hr)) return hr;
        if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
            winrt::com_ptr<IMFMediaBuffer> buffer;
            winrt::check_hresult(MFCreateSample(sample.put()));
            winrt::check_hresult(MFCreateMemoryBuffer(info.cbSize, buffer.put()));
            winrt::check_hresult(sample->AddBuffer(buffer.get()));
        }
        MFT_OUTPUT_DATA_BUFFER entry{output, sample.get(), 0, nullptr};
        DWORD status{};
        hr = mft->ProcessOutput(0, 1, &entry, &status);
        if (entry.pEvents) entry.pEvents->Release();
        if (entry.pSample != sample.get()) sample.attach(entry.pSample);
        return hr;
    }

    winrt::com_ptr<IMFTransform> mft;
    DWORD input{};
    DWORD output{};
private:
    winrt::com_ptr<IMFDXGIDeviceManager> manager_;
};

class EventWaiter : public winrt::implements<EventWaiter, IMFAsyncCallback> {
public:
    HRESULT __stdcall GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall Invoke(IMFAsyncResult* result) noexcept override {
        const std::lock_guard lock(mutex_);
        result_.copy_from(result);
        SetEvent(ready_.get());
        return S_OK;
    }

    winrt::com_ptr<IMFMediaEvent> Next(IMFMediaEventGenerator* generator) {
        winrt::com_ptr<IMFMediaEvent> event;
        const auto hr = generator->GetEvent(MF_EVENT_FLAG_NO_WAIT, event.put());
        if (hr != MF_E_NO_EVENTS_AVAILABLE) {
            winrt::check_hresult(hr);
            return event;
        }
        winrt::check_pointer(ready_.get());
        winrt::check_hresult(generator->BeginGetEvent(this, nullptr));
        if (WaitForSingleObject(ready_.get(), 120) != WAIT_OBJECT_0) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        const std::lock_guard lock(mutex_);
        winrt::check_hresult(generator->EndGetEvent(result_.get(), event.put()));
        result_ = nullptr;
        return event;
    }

private:
    std::mutex mutex_;
    winrt::handle ready_{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    winrt::com_ptr<IMFAsyncResult> result_;
};

winrt::com_ptr<ID3D11Texture2D> UploadNv12(ID3D11Device* device, const webrtc::I420BufferInterface& frame) {
    const auto w = frame.width();
    const auto h = frame.height();
    std::vector<uint8_t> bytes(static_cast<std::size_t>(w) * h * 3 / 2);
    if (libyuv::I420ToNV12(frame.DataY(), frame.StrideY(), frame.DataU(), frame.StrideU(), frame.DataV(), frame.StrideV(),
            bytes.data(), w, bytes.data() + static_cast<std::size_t>(w) * h, w, w, h)) {
        winrt::throw_hresult(E_INVALIDARG);
    }
    const D3D11_TEXTURE2D_DESC desc{static_cast<UINT>(w), static_cast<UINT>(h), 1, 1, DXGI_FORMAT_NV12,
        {1, 0}, D3D11_USAGE_DEFAULT, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA initial{bytes.data(), static_cast<UINT>(w), static_cast<UINT>(bytes.size())};
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(device->CreateTexture2D(&desc, &initial, texture.put()));
    return texture;
}

class MfEncoder final : public webrtc::VideoEncoder {
public:
    MfEncoder(ID3D11Device* device, std::shared_ptr<std::atomic_bool> active) : active_(std::move(active)) {
        device_.copy_from(device);
    }
    ~MfEncoder() override { Release(); }

    int InitEncode(const webrtc::VideoCodec* config, const Settings&) override {
        Release();
        if (!config) return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
        fps_ = std::clamp(config->maxFramerate, 1u, 60u);
        bitrate_ = std::max(150'000u, config->startBitrate * 1000);
        Apartment apartment;
        try {
            Open(config->width, config->height);
            return WEBRTC_VIDEO_CODEC_OK;
        } catch (const winrt::hresult_error& error) {
            RTC_LOG(LS_ERROR) << "MF H.264 encoder initialization failed: " << error.code().value;
            Release();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        active_->store(false);
        api_ = nullptr;
        generator_ = nullptr;
        transform_.reset();
        waiter_ = nullptr;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Encode(const webrtc::VideoFrame& frame, const std::vector<webrtc::VideoFrameType>* types) override {
        if (!callback_ || !transform_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        Apartment apartment;
        try {
            if (frame.width() != width_ || frame.height() != height_) Open(frame.width(), frame.height());
            const auto started = webrtc::TimeMillis();
            if (types && std::find(types->begin(), types->end(), webrtc::VideoFrameType::kVideoFrameKey) != types->end()) {
                SetNumber(api_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
            }
            const auto buffer = frame.video_frame_buffer();
            winrt::com_ptr<ID3D11Texture2D> uploaded;
            ID3D11Texture2D* texture{};
            UINT subresource{};
            if (buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
                const auto* native = static_cast<const Nv12Frame*>(buffer.get());
                texture = native->texture();
                subresource = native->subresource();
            } else {
                auto pixels = buffer->ToI420();
                if (!pixels) return WEBRTC_VIDEO_CODEC_ERROR;
                uploaded = UploadNv12(device_.get(), *pixels);
                texture = uploaded.get();
            }
            winrt::com_ptr<IMFMediaBuffer> dxgi;
            winrt::check_hresult(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, subresource, FALSE, dxgi.put()));
            winrt::com_ptr<IMFSample> input;
            winrt::check_hresult(MFCreateSample(input.put()));
            winrt::check_hresult(input->AddBuffer(dxgi.get()));
            winrt::check_hresult(input->SetSampleTime(frame.timestamp_us() * 10));
            winrt::check_hresult(input->SetSampleDuration(10'000'000 / fps_));
            while (!input_requests_) {
                if (EventType() == METransformNeedInput) ++input_requests_;
            }
            winrt::check_hresult(transform_->mft->ProcessInput(transform_->input, input.get(), 0));
            --input_requests_;
            while (true) {
                const auto event = EventType();
                if (event == METransformNeedInput) ++input_requests_;
                if (event == METransformHaveOutput) break;
            }
            winrt::com_ptr<IMFSample> sample;
            winrt::check_hresult(transform_->Pull(sample));
            winrt::check_pointer(sample.get());
            winrt::com_ptr<IMFMediaBuffer> bytes;
            winrt::check_hresult(sample->ConvertToContiguousBuffer(bytes.put()));
            BYTE* data{};
            DWORD length{};
            winrt::check_hresult(bytes->Lock(&data, nullptr, &length));
            auto payload = webrtc::EncodedImageBuffer::Create(data, length);
            bytes->Unlock();
            const auto nals = webrtc::H264::FindNaluIndices({payload->data(), payload->size()});
            if (nals.empty()) winrt::throw_hresult(MF_E_INVALID_STREAM_DATA);
            const auto idr = std::any_of(nals.begin(), nals.end(), [&](const auto& nal) {
                return (payload->data()[nal.payload_start_offset] & 31) == webrtc::H264::NaluType::kIdr;
            });
            webrtc::EncodedImage encoded;
            encoded.SetEncodedData(std::move(payload));
            encoded._encodedWidth = width_;
            encoded._encodedHeight = height_;
            encoded.SetRtpTimestamp(frame.rtp_timestamp());
            encoded.capture_time_ms_ = frame.timestamp_us() / 1000;
            encoded.SetEncodeTime(started, webrtc::TimeMillis());
            encoded.SetFrameType(idr ? webrtc::VideoFrameType::kVideoFrameKey : webrtc::VideoFrameType::kVideoFrameDelta);
            webrtc::CodecSpecificInfo metadata{};
            metadata.codecType = webrtc::kVideoCodecH264;
            metadata.codecSpecific.H264.packetization_mode = webrtc::H264PacketizationMode::NonInterleaved;
            metadata.codecSpecific.H264.temporal_idx = webrtc::kNoTemporalIdx;
            metadata.codecSpecific.H264.idr_frame = idr;
            callback_->OnEncodedImage(encoded, &metadata);
            return WEBRTC_VIDEO_CODEC_OK;
        } catch (const winrt::hresult_error& error) {
            RTC_LOG(LS_ERROR) << "MF H.264 encode failed: " << error.code().value;
            Release();
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    void SetRates(const RateControlParameters& rates) override {
        const auto target = rates.bitrate.get_sum_bps();
        if (!target) return;
        bitrate_ = std::max(150'000u, target);
        SetNumber(api_.get(), CODECAPI_AVEncCommonMeanBitRate, bitrate_);
        if (variable_bitrate_) SetNumber(api_.get(), CODECAPI_AVEncCommonMaxBitRate, bitrate_);
    }

    EncoderInfo GetEncoderInfo() const override {
        EncoderInfo info;
        info.implementation_name = "Media Foundation H.264 (hardware)";
        info.supports_native_handle = true;
        info.is_hardware_accelerated = true;
        info.requested_resolution_alignment = 2;
        return info;
    }

private:
    void Open(int width, int height) {
        if (width <= 0 || height <= 0 || width > static_cast<int>(width_limit) || height > static_cast<int>(height_limit)
                || width % 2 || height % 2) winrt::throw_hresult(E_INVALIDARG);
        Release();
        const auto activation = EncoderActivation(device_.get());
        winrt::check_pointer(activation.get());
        winrt::com_ptr<IMFTransform> mft;
        winrt::check_hresult(activation->ActivateObject(__uuidof(IMFTransform), mft.put_void()));
        activation->DetachObject();
        winrt::com_ptr<IMFAttributes> attributes;
        winrt::check_hresult(mft->GetAttributes(attributes.put()));
        winrt::check_hresult(attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        transform_ = std::make_unique<Transform>(std::move(mft), device_.get());
        generator_ = transform_->mft.as<IMFMediaEventGenerator>();
        api_ = transform_->mft.try_as<ICodecAPI>();
        Enable(api_.get(), CODECAPI_AVLowLatencyMode);
        variable_bitrate_ = SetNumber(api_.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
        if (variable_bitrate_) {
            SetNumber(api_.get(), CODECAPI_AVEncCommonMaxBitRate, bitrate_);
        } else {
            SetNumber(api_.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        }
        SetNumber(api_.get(), CODECAPI_AVEncCommonBufferSize, bitrate_ / (8 * fps_));
        SetNumber(api_.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetNumber(api_.get(), CODECAPI_AVEncMPVGOPSize, fps_ * 4);
        auto h264 = MediaType(MFVideoFormat_H264, width, height, fps_);
        winrt::check_hresult(h264->SetUINT32(MF_MT_AVG_BITRATE, bitrate_));
        winrt::check_hresult(h264->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base));
        winrt::check_hresult(transform_->mft->SetOutputType(transform_->output, h264.get(), 0));
        const auto nv12 = MediaType(MFVideoFormat_NV12, width, height, fps_);
        winrt::check_hresult(transform_->mft->SetInputType(transform_->input, nv12.get(), 0));
        transform_->Start();
        waiter_ = winrt::make_self<EventWaiter>();
        input_requests_ = 0;
        width_ = width;
        height_ = height;
        active_->store(true);
    }

    MediaEventType EventType() {
        const auto event = waiter_->Next(generator_.get());
        HRESULT status{};
        winrt::check_hresult(event->GetStatus(&status));
        winrt::check_hresult(status);
        MediaEventType type{};
        winrt::check_hresult(event->GetType(&type));
        return type;
    }

    winrt::com_ptr<ID3D11Device> device_;
    std::shared_ptr<std::atomic_bool> active_;
    std::unique_ptr<Transform> transform_;
    winrt::com_ptr<IMFMediaEventGenerator> generator_;
    winrt::com_ptr<ICodecAPI> api_;
    winrt::com_ptr<EventWaiter> waiter_;
    webrtc::EncodedImageCallback* callback_{};
    unsigned fps_{30};
    unsigned bitrate_{2'000'000};
    unsigned input_requests_{};
    bool variable_bitrate_{};
    int width_{};
    int height_{};
};

class MfDecoder final : public webrtc::VideoDecoder {
public:
    explicit MfDecoder(ID3D11Device* device) { device_.copy_from(device); }

    bool Configure(const Settings&) override {
        Apartment apartment;
        try {
            winrt::com_ptr<IMFTransform> mft;
            winrt::check_hresult(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IMFTransform), mft.put_void()));
            winrt::com_ptr<IMFAttributes> attributes;
            winrt::check_hresult(mft->GetAttributes(attributes.put()));
            attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
            attributes->SetUINT32(CODECAPI_AVDecVideoAcceleration_H264, TRUE);
            const auto api = mft.try_as<ICodecAPI>();
            Enable(api.get(), CODECAPI_AVLowLatencyMode);
            Enable(api.get(), CODECAPI_AVDecVideoAcceleration_H264);
            transform_ = std::make_unique<Transform>(std::move(mft), device_.get());
            const auto input = MediaType(MFVideoFormat_H264);
            winrt::check_hresult(transform_->mft->SetInputType(transform_->input, input.get(), 0));
            ChooseOutput();
            transform_->Start();
            return true;
        } catch (const winrt::hresult_error& error) {
            RTC_LOG(LS_ERROR) << "MF H.264 decoder initialization failed: " << error.code().value;
            Release();
            return false;
        }
    }

    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback* callback) override {
        callback_ = callback;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Release() override {
        transform_.reset();
        frames_.clear();
        hardware_ = false;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    int32_t Decode(const webrtc::EncodedImage& encoded, int64_t render_time_ms) override {
        if (!transform_ || !callback_) return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
        if (!encoded.size() || encoded.size() > 8 * 1024 * 1024) return WEBRTC_VIDEO_CODEC_ERROR;
        Apartment apartment;
        try {
            if (frames_.size() >= 16) winrt::throw_hresult(MF_E_INVALID_STREAM_DATA);
            winrt::com_ptr<IMFSample> sample;
            winrt::com_ptr<IMFMediaBuffer> compressed;
            winrt::check_hresult(MFCreateSample(sample.put()));
            winrt::check_hresult(MFCreateMemoryBuffer(static_cast<DWORD>(encoded.size()), compressed.put()));
            BYTE* data{};
            winrt::check_hresult(compressed->Lock(&data, nullptr, nullptr));
            std::memcpy(data, encoded.data(), encoded.size());
            compressed->Unlock();
            winrt::check_hresult(compressed->SetCurrentLength(static_cast<DWORD>(encoded.size())));
            winrt::check_hresult(sample->AddBuffer(compressed.get()));
            winrt::check_hresult(sample->SetSampleTime(next_sample_time_));
            winrt::check_hresult(sample->SetSampleDuration(166'667));
            winrt::check_hresult(transform_->mft->ProcessInput(transform_->input, sample.get(), 0));
            frames_.push_back({next_sample_time_, encoded.RtpTimestamp(), render_time_ms});
            next_sample_time_ += 166'667;
            bool decoded = false;
            while (true) {
                winrt::com_ptr<IMFSample> output;
                const auto hr = transform_->Pull(output);
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                    ChooseOutput();
                    continue;
                }
                winrt::check_hresult(hr);
                winrt::check_pointer(output.get());
                Emit(std::move(output));
                decoded = true;
            }
            return decoded ? WEBRTC_VIDEO_CODEC_OK : WEBRTC_VIDEO_CODEC_NO_OUTPUT;
        } catch (const winrt::hresult_error& error) {
            RTC_LOG(LS_ERROR) << "MF H.264 decode failed: " << error.code().value;
            frames_.clear();
            transform_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            transform_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            return WEBRTC_VIDEO_CODEC_ERROR;
        }
    }

    DecoderInfo GetDecoderInfo() const override {
        return {hardware_.load() ? "Media Foundation H.264 (DXVA)" : "Media Foundation H.264", hardware_.load()};
    }

private:
    struct FrameTime { LONGLONG time; uint32_t rtp; int64_t render_ms; };

    void ChooseOutput() {
        for (DWORD i = 0;; ++i) {
            winrt::com_ptr<IMFMediaType> type;
            winrt::check_hresult(transform_->mft->GetOutputAvailableType(transform_->output, i, type.put()));
            GUID subtype{};
            if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
            if (FAILED(transform_->mft->SetOutputType(transform_->output, type.get(), 0))) continue;
            winrt::check_hresult(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &coded_width_, &coded_height_));
            width_ = coded_width_;
            height_ = coded_height_;
            MFVideoArea aperture{};
            if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<BYTE*>(&aperture),
                    sizeof(aperture), nullptr)) && !aperture.OffsetX.value && !aperture.OffsetY.value
                    && aperture.Area.cx > 0 && aperture.Area.cy > 0) {
                width_ = static_cast<UINT>(aperture.Area.cx);
                height_ = static_cast<UINT>(aperture.Area.cy);
            }
            if (!width_ || !height_ || width_ > width_limit || height_ > height_limit
                    || width_ > coded_width_ || height_ > coded_height_) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
            return;
        }
    }

    void Emit(winrt::com_ptr<IMFSample> sample) {
        if (frames_.empty()) return;
        LONGLONG time{};
        if (SUCCEEDED(sample->GetSampleTime(&time))) {
            while (frames_.size() > 1 && frames_.front().time < time) frames_.pop_front();
        }
        const auto timestamp = frames_.front();
        frames_.pop_front();
        winrt::com_ptr<IMFMediaBuffer> media;
        winrt::check_hresult(sample->GetBufferByIndex(0, media.put()));
        webrtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer;
        if (auto gpu = media.try_as<IMFDXGIBuffer>()) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            UINT subresource{};
            winrt::check_hresult(gpu->GetResource(__uuidof(ID3D11Texture2D), texture.put_void()));
            winrt::check_hresult(gpu->GetSubresourceIndex(&subresource));
            buffer = webrtc::make_ref_counted<Nv12Frame>(std::move(texture), subresource,
                static_cast<int>(width_), static_cast<int>(height_), sample.as<IUnknown>());
            hardware_ = true;
        } else {
            hardware_ = false;
            auto pixels = webrtc::I420Buffer::Create(width_, height_);
            std::vector<BYTE> linear;
            if (auto planar = media.try_as<IMF2DBuffer>()) {
                DWORD length{};
                winrt::check_hresult(planar->GetContiguousLength(&length));
                linear.resize(length);
                winrt::check_hresult(planar->ContiguousCopyTo(linear.data(), length));
            } else {
                BYTE* data{};
                DWORD length{};
                winrt::check_hresult(media->Lock(&data, nullptr, &length));
                linear.assign(data, data + length);
                media->Unlock();
            }
            const auto y_size = static_cast<std::size_t>(coded_width_) * coded_height_;
            if (linear.size() < y_size * 3 / 2 || libyuv::NV12ToI420(linear.data(), coded_width_, linear.data() + y_size,
                    coded_width_, pixels->MutableDataY(), pixels->StrideY(), pixels->MutableDataU(), pixels->StrideU(),
                    pixels->MutableDataV(), pixels->StrideV(), width_, height_)) winrt::throw_hresult(MF_E_INVALIDMEDIATYPE);
            buffer = std::move(pixels);
        }
        auto frame = webrtc::VideoFrame::Builder().set_video_frame_buffer(std::move(buffer))
            .set_timestamp_rtp(timestamp.rtp).set_timestamp_ms(timestamp.render_ms).build();
        callback_->Decoded(frame);
    }

    winrt::com_ptr<ID3D11Device> device_;
    std::unique_ptr<Transform> transform_;
    webrtc::DecodedImageCallback* callback_{};
    std::deque<FrameTime> frames_;
    LONGLONG next_sample_time_{};
    UINT coded_width_{}, coded_height_{}, width_{}, height_{};
    std::atomic_bool hardware_{};
};

class EncoderFactory final : public webrtc::VideoEncoderFactory {
public:
    EncoderFactory(ID3D11Device* device, std::shared_ptr<std::atomic_bool> active) : active_(std::move(active)) {
        device_.copy_from(device);
    }
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {H264Format(), webrtc::SdpVideoFormat("VP8")};
    }
    std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
        if (format.name == "H264") return std::make_unique<MfEncoder>(device_.get(), active_);
        return vp8_.Create(env, format);
    }
private:
    winrt::com_ptr<ID3D11Device> device_;
    std::shared_ptr<std::atomic_bool> active_;
    Vp8EncoderFactory vp8_;
};

class DecoderFactory final : public webrtc::VideoDecoderFactory {
public:
    explicit DecoderFactory(ID3D11Device* device) { device_.copy_from(device); }
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        return {H264Format(), webrtc::SdpVideoFormat("VP8")};
    }
    std::unique_ptr<webrtc::VideoDecoder> Create(const webrtc::Environment& env, const webrtc::SdpVideoFormat& format) override {
        if (format.name == "H264") return std::make_unique<MfDecoder>(device_.get());
        return vp8_.Create(env, format);
    }
private:
    winrt::com_ptr<ID3D11Device> device_;
    Vp8DecoderFactory vp8_;
};

} // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> CreateVideoEncoderFactory(ID3D11Device* device,
    std::shared_ptr<std::atomic_bool> active) {
    try {
        if (device && MediaFoundationAvailable() && EncoderActivation(device)) {
            return std::make_unique<EncoderFactory>(device, std::move(active));
        }
    } catch (const winrt::hresult_error&) {
    }
    return std::make_unique<Vp8EncoderFactory>();
}

std::unique_ptr<webrtc::VideoDecoderFactory> CreateVideoDecoderFactory(ID3D11Device* device) {
    if (device && MediaFoundationAvailable() && DecoderAvailable(device)) return std::make_unique<DecoderFactory>(device);
    return std::make_unique<Vp8DecoderFactory>();
}

} // namespace p2premote
