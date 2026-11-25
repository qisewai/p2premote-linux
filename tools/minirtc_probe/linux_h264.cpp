#include "linux_h264.h"

#include <libyuv.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace minirtc {
namespace {

enum AVPixelFormat selectVaapiFormat(AVCodecContext*,
                                     const enum AVPixelFormat* formats) {
  for (const auto* it = formats; *it != AV_PIX_FMT_NONE; ++it) {
    if (*it == AV_PIX_FMT_VAAPI) return AV_PIX_FMT_VAAPI;
  }
  return formats[0];
}

class LinuxH264Decoder final : public MediaCodec {
 public:
  LinuxH264Decoder(std::shared_ptr<SystemClock> clock,
                   bool hardware_acceleration)
      : clock_(std::move(clock)),
        hardware_acceleration_(hardware_acceleration) {}

  ~LinuxH264Decoder() override {
    if (packet_) av_packet_free(&packet_);
    if (sw_frame_) av_frame_free(&sw_frame_);
    if (frame_) av_frame_free(&frame_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (hw_device_) av_buffer_unref(&hw_device_);
  }

  int Init() override {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
      LOG_WARN("FFmpeg H.264 decoder not found");
      return -1;
    }

    ctx_ = avcodec_alloc_context3(codec);
    if (!ctx_) return -1;

    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

    if (hardware_acceleration_) {
      if (av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                                 nullptr, 0) < 0) {
        av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/renderD128", nullptr, 0);
      }
      if (hw_device_) {
        ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
        ctx_->get_format = selectVaapiFormat;
        ctx_->thread_count = 1;
        using_vaapi_ = true;
      }
    }

    if (!using_vaapi_) {
      ctx_->thread_type = FF_THREAD_SLICE;
      ctx_->thread_count = 4;
    }

    if (avcodec_open2(ctx_, codec, nullptr) < 0) {
      LOG_WARN("Failed to open FFmpeg H.264 decoder (vaapi={})", using_vaapi_);
      if (using_vaapi_) {
        avcodec_free_context(&ctx_);
        av_buffer_unref(&hw_device_);
        using_vaapi_ = false;
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) return -1;
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx_->thread_type = FF_THREAD_SLICE;
        ctx_->thread_count = 4;
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
          LOG_WARN("Failed to open FFmpeg software H.264 decoder");
          return -1;
        }
      } else {
        return -1;
      }
    }

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    if (!packet_ || !frame_ || !sw_frame_) return -1;

    LOG_INFO("Linux H.264 decoder initialized [{}]",
             using_vaapi_ ? "FFmpeg VA-API" : "FFmpeg software");
    return 0;
  }

  int Decode(std::unique_ptr<ReceivedFrame> received_frame,
             std::function<void(const DecodedFrame*)> on_receive_decoded_frame)
      override {
    if (!ctx_ || !received_frame || !received_frame->Buffer() ||
        received_frame->Size() == 0) {
      return -1;
    }

    const int size = static_cast<int>(received_frame->Size());
    if (size <= 0 || av_new_packet(packet_, size) < 0) return -1;
    std::memcpy(packet_->data, received_frame->Buffer(),
                static_cast<size_t>(size));

    int ret = avcodec_send_packet(ctx_, packet_);
    av_packet_unref(packet_);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
      LOG_WARN("FFmpeg H.264 send_packet failed: {}", ret);
      return -1;
    }

    while (true) {
      ret = avcodec_receive_frame(ctx_, frame_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
      if (ret < 0) {
        LOG_WARN("FFmpeg H.264 receive_frame failed: {}", ret);
        return -1;
      }

      AVFrame* cpu = frame_;
      if (frame_->format == AV_PIX_FMT_VAAPI) {
        av_frame_unref(sw_frame_);
        sw_frame_->format = AV_PIX_FMT_NV12;
        if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
          av_frame_unref(frame_);
          LOG_WARN("VA-API frame transfer failed");
          return -1;
        }
        cpu = sw_frame_;
      }

      const int width = cpu->width;
      const int height = cpu->height;
      if (width <= 0 || height <= 0 || ((width | height) & 1)) {
        av_frame_unref(frame_);
        continue;
      }

      const size_t y_size = static_cast<size_t>(width) * height;
      const size_t nv12_size = y_size + y_size / 2;
      if (nv12_.size() != nv12_size) nv12_.resize(nv12_size);

      uint8_t* dst_y = nv12_.data();
      uint8_t* dst_uv = dst_y + y_size;
      if (cpu->format == AV_PIX_FMT_NV12) {
        libyuv::CopyPlane(cpu->data[0], cpu->linesize[0], dst_y, width, width,
                          height);
        libyuv::CopyPlane(cpu->data[1], cpu->linesize[1], dst_uv, width, width,
                          height / 2);
      } else if (cpu->format == AV_PIX_FMT_YUV420P ||
                 cpu->format == AV_PIX_FMT_YUVJ420P) {
        libyuv::I420ToNV12(cpu->data[0], cpu->linesize[0], cpu->data[1],
                           cpu->linesize[1], cpu->data[2], cpu->linesize[2],
                           dst_y, width, dst_uv, width, width, height);
      } else {
        av_frame_unref(frame_);
        LOG_WARN("Unsupported decoded pixel format: {}", cpu->format);
        return -1;
      }

      if (!decoded_frame_ ||
          decoded_frame_->Width() != static_cast<uint32_t>(width) ||
          decoded_frame_->Height() != static_cast<uint32_t>(height)) {
        decoded_frame_ = std::make_unique<DecodedFrame>(
            nv12_size, static_cast<uint32_t>(width),
            static_cast<uint32_t>(height));
      }
      decoded_frame_->UpdateBuffer(nv12_.data(), nv12_size);
      decoded_frame_->SetWidth(static_cast<uint32_t>(width));
      decoded_frame_->SetHeight(static_cast<uint32_t>(height));
      decoded_frame_->SetDecodedWidth(static_cast<uint32_t>(width));
      decoded_frame_->SetDecodedHeight(static_cast<uint32_t>(height));
      decoded_frame_->SetReceivedTimestamp(received_frame->ReceivedTimestamp());
      decoded_frame_->SetCapturedTimestamp(received_frame->CapturedTimestamp());
      if (clock_) decoded_frame_->SetDecodedTimestamp(clock_->CurrentTime());
      if (on_receive_decoded_frame) {
        on_receive_decoded_frame(decoded_frame_.get());
      }
      av_frame_unref(frame_);
    }

    return 0;
  }

  std::string GetDecoderName() const override {
    return using_vaapi_ ? "FFmpeg-VAAPI" : "FFmpeg-H264";
  }

 private:
  std::shared_ptr<SystemClock> clock_;
  bool hardware_acceleration_{};
  bool using_vaapi_{};
  AVBufferRef* hw_device_{};
  AVCodecContext* ctx_{};
  AVPacket* packet_{};
  AVFrame* frame_{};
  AVFrame* sw_frame_{};
  std::vector<uint8_t> nv12_;
  std::unique_ptr<DecodedFrame> decoded_frame_;
};

}  // namespace

std::unique_ptr<MediaCodec> CreateLinuxH264Decoder(
    std::shared_ptr<SystemClock> clock, bool hardware_acceleration) {
  return std::make_unique<LinuxH264Decoder>(std::move(clock),
                                            hardware_acceleration);
}

}  // namespace minirtc
