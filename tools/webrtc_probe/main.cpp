#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_modular_peer_connection_factory.h"
#include "api/enable_media.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/peer_connection_interface.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "webrtc_vp8.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/ssl_adapter.h"
#include "rtc_base/thread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int width = 1280;
constexpr int height = 720;
constexpr int luma = 96;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

using p2premote::Vp8DecoderFactory;
using p2premote::Vp8EncoderFactory;

class NativeFrame : public webrtc::VideoFrameBuffer {
public:
    explicit NativeFrame(webrtc::scoped_refptr<webrtc::I420BufferInterface> fallback)
        : fallback_(std::move(fallback)) {}

    Type type() const override { return Type::kNative; }
    int width() const override { return fallback_->width(); }
    int height() const override { return fallback_->height(); }

    webrtc::scoped_refptr<webrtc::I420BufferInterface> ToI420() override {
        used_fallback = true;
        return fallback_;
    }

    bool used_fallback = false;

private:
    webrtc::scoped_refptr<webrtc::I420BufferInterface> fallback_;
};

class EncodedOutput final : public webrtc::EncodedImageCallback {
public:
    Result OnEncodedImage(const webrtc::EncodedImage& image, const webrtc::CodecSpecificInfo*) override {
        frame = image;
        return Result(Result::OK);
    }

    void OnFrameDropped(std::uint32_t, int, bool) override { dropped = true; }

    std::optional<webrtc::EncodedImage> frame;
    bool dropped = false;
};

class DecodedOutput final : public webrtc::DecodedImageCallback {
public:
    int32_t Decoded(webrtc::VideoFrame& image) override {
        frame = image;
        return WEBRTC_VIDEO_CODEC_OK;
    }

    std::optional<webrtc::VideoFrame> frame;
};

void VerifyVp8() {
    const auto environment = webrtc::CreateEnvironment();
    Vp8EncoderFactory encoder_factory;
    Vp8DecoderFactory decoder_factory;
    const auto encode_formats = encoder_factory.GetSupportedFormats();
    const auto decode_formats = decoder_factory.GetSupportedFormats();
    Check(!encode_formats.empty() && !decode_formats.empty(), "The built-in factories must advertise VP8");

    EncodedOutput encoded;
    DecodedOutput decoded;
    const auto encoder = encoder_factory.Create(environment, encode_formats.front());
    const auto decoder = decoder_factory.Create(environment, decode_formats.front());
    Check(encoder && decoder, "The custom factories must create a VP8 encoder and decoder");

    webrtc::VideoCodec codec{};
    codec.codecType = webrtc::kVideoCodecVP8;
    *codec.VP8() = webrtc::VideoEncoder::GetDefaultVp8Settings();
    codec.width = width;
    codec.height = height;
    codec.startBitrate = 1200;
    codec.maxBitrate = 2000;
    codec.minBitrate = 100;
    codec.maxFramerate = 30;
    const webrtc::VideoEncoder::Settings encoder_settings{webrtc::VideoEncoder::Capabilities{false}, 1, 1200};
    Check(encoder->InitEncode(&codec, encoder_settings) == WEBRTC_VIDEO_CODEC_OK, "VP8 InitEncode failed");
    Check(encoder->RegisterEncodeCompleteCallback(&encoded) == WEBRTC_VIDEO_CODEC_OK, "Cannot register encoder callback");

    auto pixels = webrtc::I420Buffer::Create(width, height);
    webrtc::I420Buffer::SetBlack(pixels.get());
    std::fill_n(pixels->MutableDataY(), pixels->StrideY() * height, static_cast<std::uint8_t>(luma));
    auto native = webrtc::make_ref_counted<NativeFrame>(pixels);
    auto frame = webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(native)
        .set_timestamp_us(1'000'000)
        .build();

    const std::vector frame_types{webrtc::VideoFrameType::kVideoFrameKey};
    Check(encoder->Encode(frame, &frame_types) == WEBRTC_VIDEO_CODEC_OK, "VP8 Encode failed");
    Check(native->used_fallback, "The VP8 encoder must call the kNative buffer's ToI420 fallback");
    Check(!encoded.dropped && encoded.frame && encoded.frame->size() > 0, "No encoded VP8 frame received");

    webrtc::VideoDecoder::Settings settings;
    settings.set_codec_type(webrtc::kVideoCodecVP8);
    Check(decoder->Configure(settings), "VP8 decoder configuration failed");
    Check(decoder->RegisterDecodeCompleteCallback(&decoded) == WEBRTC_VIDEO_CODEC_OK, "Cannot register decoder callback");
    Check(decoder->Decode(*encoded.frame, 0) == WEBRTC_VIDEO_CODEC_OK, "VP8 Decode failed");
    Check(decoded.frame && decoded.frame->width() == width && decoded.frame->height() == height,
        "Decoded VP8 frame does not match the input resolution");

    const auto picture = decoded.frame->video_frame_buffer()->ToI420();
    Check(picture && std::abs(picture->DataY()[height / 2 * picture->StrideY() + width / 2] - luma) <= 8,
        "Decoded VP8 luma does not match the source");
    std::cout << "PASS: 1280x720 VP8 encode/decode, custom factories, kNative -> I420 ("
              << encoded.frame->size() << " encoded bytes)" << std::endl;
}

class ConnectionObserver final : public webrtc::PeerConnectionObserver {
public:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
    void OnIceCandidate(const webrtc::IceCandidate*) override {}
};

class OfferObserver : public webrtc::CreateSessionDescriptionObserver {
public:
    std::future<std::string> Result() { return result_.get_future(); }

    void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
        const std::unique_ptr<webrtc::SessionDescriptionInterface> offer(description);
        std::string sdp;
        if (offer && offer->ToString(&sdp)) {
            result_.set_value(std::move(sdp));
        } else {
            OnFailure(webrtc::RTCError(webrtc::RTCErrorType::INTERNAL_ERROR, "Cannot serialize SDP offer"));
        }
    }

    void OnFailure(webrtc::RTCError error) override {
        result_.set_exception(std::make_exception_ptr(std::runtime_error(error.message())));
    }

private:
    std::promise<std::string> result_;
};

void VerifyPeerConnection() {
    const auto signaling = webrtc::Thread::Create();
    Check(signaling->Start(), "Cannot start the signaling thread");

    webrtc::PeerConnectionFactoryDependencies dependencies;
    dependencies.signaling_thread = signaling.get();
    dependencies.audio_encoder_factory = webrtc::CreateBuiltinAudioEncoderFactory();
    dependencies.audio_decoder_factory = webrtc::CreateBuiltinAudioDecoderFactory();
    dependencies.video_encoder_factory = std::make_unique<Vp8EncoderFactory>();
    dependencies.video_decoder_factory = std::make_unique<Vp8DecoderFactory>();
    webrtc::EnableMedia(dependencies);
    const auto factory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
    Check(factory != nullptr, "Cannot create PeerConnection factory");

    ConnectionObserver observer;
    auto created = factory->CreatePeerConnectionOrError({}, webrtc::PeerConnectionDependencies(&observer));
    Check(created.ok(), created.ok() ? "" : created.error().message());
    const auto connection = created.MoveValue();

    webrtc::RtpTransceiverInit video;
    video.direction = webrtc::RtpTransceiverDirection::kRecvOnly;
    const auto transceiver = connection->AddTransceiver(webrtc::MediaType::VIDEO, video);
    Check(transceiver.ok(), transceiver.ok() ? "" : transceiver.error().message());
    transceiver.value()->receiver()->SetJitterBufferMinimumDelay(0.0);

    const auto offer = webrtc::make_ref_counted<OfferObserver>();
    auto result = offer->Result();
    connection->CreateOffer(offer.get(), {});
    Check(result.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "SDP offer timed out");
    const auto sdp = result.get();
    Check(sdp.find("m=video ") != std::string::npos && sdp.find(" VP8/90000") != std::string::npos
            && sdp.find("a=recvonly") != std::string::npos,
        "SDP offer does not contain a receive-only VP8 video track");
    connection->Close();
    std::cout << "PASS: PeerConnection, receive-only VP8 SDP, minimum jitter-buffer delay and shutdown\n";
}

} // namespace

int main() {
    if (!webrtc::InitializeSSL()) {
        std::cerr << "FAIL: WebRTC SSL initialization failed\n";
        return 1;
    }
    int result = 0;
    try {
        VerifyVp8();
        VerifyPeerConnection();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        result = 1;
    }
    if (!webrtc::CleanupSSL()) {
        std::cerr << "FAIL: WebRTC SSL cleanup failed\n";
        result = 1;
    }
    return result;
}
