# Run after the managed and latency adaptations. Restore independent sources
# from the pinned revision so repeated builds do not accumulate edits.
function Set-HardwareSource([string]$file, [string]$text) {
    [IO.File]::WriteAllText((Join-Path $source $file), $text)
}

$dir=Join-Path $source 'src/media/video/windows'
[void](New-Item -ItemType Directory -Path $dir -Force)
foreach ($name in 'windows_h264.cpp','windows_h264.h','dxgi_nv12_frame.h') {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $dir -Force
}

$file='src/api/minirtc.h'
$text=[IO.File]::ReadAllText((Join-Path $source $file))
$text=Replace-MiniRtcSource $text '  MiniRtcNativeVideoFrameAndroidMediaCodec = 4, ///< Output-only Surface buffer.' @'
  MiniRtcNativeVideoFrameAndroidMediaCodec = 4, ///< Output-only Surface buffer.
  MiniRtcNativeVideoFrameD3d11Nv12 = 5, ///< ID3D11Texture2D with NV12 data.
'@
$text=Replace-MiniRtcSource $text '  MiniRtcCudaNv12Frame cuda_nv12;' @'
  MiniRtcCudaNv12Frame cuda_nv12;
  struct { void* texture; uint32_t subresource; } d3d11_nv12;
'@
$text=Replace-MiniRtcSource $text '} Params;' "  void* windows_d3d11_device; // Borrowed from the local session.`n} Params;"
Set-HardwareSource $file $text

$file='src/frame/native_video_frame.h'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '    case MiniRtcNativeVideoFrameNone:' @'
    case MiniRtcNativeVideoFrameD3d11Nv12:
      return frame->payload.d3d11_nv12.texture ? frame : nullptr;
    case MiniRtcNativeVideoFrameNone:
'@
Set-HardwareSource $file $text

foreach ($file in 'src/pc/peer_connection.h','src/pc/connection_interface.h') {
    $text=if ($file.EndsWith('peer_connection.h')) {[IO.File]::ReadAllText((Join-Path $source $file))} else {Get-UpstreamText $file}
    Set-HardwareSource $file (Replace-MiniRtcSource $text '  bool native_video_output;' "  bool native_video_output;`n  void* windows_d3d11_device;")
}
$file='src/rtc/minirtc_inner.cpp'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '  peer_ptr->pc_params.use_cfg_file = params->use_cfg_file;' @'
  peer_ptr->pc_params.use_cfg_file = params->use_cfg_file;
  peer_ptr->pc_params.windows_d3d11_device = params->windows_d3d11_device;
'@
Set-HardwareSource $file $text
$file='src/pc/peer_connection.cpp'
$text=Replace-MiniRtcSource ([IO.File]::ReadAllText((Join-Path $source $file))) '  connection_info_.native_video_output = native_video_output_;' @'
  connection_info_.native_video_output = native_video_output_;
  connection_info_.windows_d3d11_device = params.windows_d3d11_device;
'@
Set-HardwareSource $file $text
$file='src/pc/minirtc_connection.cpp'
$text=Replace-MiniRtcSource ([IO.File]::ReadAllText((Join-Path $source $file))) '          video_payload_types_, audio_payload_types_);' @'
          video_payload_types_, audio_payload_types_, info_.windows_d3d11_device);
'@
Set-HardwareSource $file $text

$file='src/transport/ice_transport.h'
$text=[IO.File]::ReadAllText((Join-Path $source $file))
$text=Replace-MiniRtcSource $text '                           std::vector<int>& audio_payload_types);' @'
                           std::vector<int>& audio_payload_types,
                           void* windows_d3d11_device = nullptr);
'@
$text=Replace-MiniRtcSource $text '  bool native_video_output_ = false;' "  bool native_video_output_ = false;`n  void* windows_d3d11_device_ = nullptr;"
Set-HardwareSource $file $text
$file='src/transport/ice_transport.cpp'
$text=[IO.File]::ReadAllText((Join-Path $source $file))
$text=Replace-MiniRtcSource $text '    std::vector<int>& audio_payload_types) {' @'
    std::vector<int>& audio_payload_types, void* windows_d3d11_device) {
  windows_d3d11_device_ = windows_d3d11_device;
'@
$text=Replace-MiniRtcSource $text 'on_receive_video_, on_receive_audio_, on_receive_data_, user_data_);' @'
on_receive_video_, on_receive_audio_, on_receive_data_, user_data_, windows_d3d11_device_);
'@
Set-HardwareSource $file $text

$file='src/transport/ice_transport_controller.h'
$text=[IO.File]::ReadAllText((Join-Path $source $file))
$text=Replace-MiniRtcSource $text '              OnReceiveData on_receive_data, void* user_data);' @'
              OnReceiveData on_receive_data, void* user_data,
              void* windows_d3d11_device = nullptr);
'@
$text=Replace-MiniRtcSource $text '  bool native_video_output_;' "  bool native_video_output_;`n  void* windows_d3d11_device_ = nullptr;"
Set-HardwareSource $file $text
$file='src/transport/ice_transport_controller.cpp'
$text=[IO.File]::ReadAllText((Join-Path $source $file)).Replace("`r`n", "`n")
$text=Replace-MiniRtcSource $text '                                    void* user_data) {' @'
                                    void* user_data, void* windows_d3d11_device) {
  windows_d3d11_device_ = windows_d3d11_device;
'@
$text=Replace-MiniRtcSource $text '          self->video_codec_type_);' '          self->video_codec_type_, self->windows_d3d11_device_);'
$text=Replace-MiniRtcSource $text '                  clock, media_config_, hardware_acceleration, codec_type);' @'
                  clock, media_config_, hardware_acceleration, codec_type,
                  windows_d3d11_device_);
'@
$text=Replace-MiniRtcSource $text '              clock, hardware_acceleration, codec_type, native_video_output_);' @'
              clock, hardware_acceleration, codec_type, native_video_output_,
              windows_d3d11_device_);
'@
$text=Replace-MiniRtcSource $text "#if defined(__APPLE__) || defined(__ANDROID__)`n    ret = CreateStreamCodecs(clock, hardware_acceleration_,`n                             VideoCodecType::H264);" @'
#if defined(__APPLE__) || defined(__ANDROID__) || defined(_WIN32)
    ret = CreateStreamCodecs(clock, hardware_acceleration_,
                             VideoCodecType::H264);
'@
Set-HardwareSource $file $text

$file='src/media/video/encode/video_encoder_factory.h'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '      bool hardware_acceleration, VideoCodecType codec_type);' @'
      bool hardware_acceleration, VideoCodecType codec_type,
      void* windows_d3d11_device = nullptr);
'@
Set-HardwareSource $file $text
$file='src/media/video/encode/video_encoder_factory.cpp'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '#include "video_encoder_factory.h"' @'
#include "video_encoder_factory.h"
#if defined(_WIN32)
#include "../windows/windows_h264.h"
#endif
'@
$text=Replace-MiniRtcSource $text '    bool hardware_acceleration, VideoCodecType codec_type) {' @'
    bool hardware_acceleration, VideoCodecType codec_type,
    void* windows_d3d11_device) {
#if defined(_WIN32)
  if (hardware_acceleration && windows_d3d11_device && codec_type == VideoCodecType::H264) {
    auto encoder = CreateWindowsH264Encoder(clock, windows_d3d11_device);
    return encoder->Init(config) == 0 ? std::move(encoder) : nullptr;
  }
#endif
'@
Set-HardwareSource $file $text
$file='src/media/video/decode/video_decoder_factory.h'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '                     bool native_video_output = false);' @'
                     bool native_video_output = false,
                     void* windows_d3d11_device = nullptr);
'@
Set-HardwareSource $file $text
$file='src/media/video/decode/video_decoder_factory.cpp'
$text=Replace-MiniRtcSource (Get-UpstreamText $file) '#include "video_decoder_factory.h"' @'
#include "video_decoder_factory.h"
#if defined(_WIN32)
#include "../windows/windows_h264.h"
#endif
'@
$text=Replace-MiniRtcSource $text '                                        bool native_video_output) {' @'
                                        bool native_video_output,
                                        void* windows_d3d11_device) {
#if defined(_WIN32)
  if (hardware_acceleration && windows_d3d11_device && native_video_output && codec_type == VideoCodecType::H264) {
    return CreateWindowsH264Decoder(clock, windows_d3d11_device);
  }
#endif
'@
Set-HardwareSource $file $text
