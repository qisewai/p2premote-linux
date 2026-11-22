function Replace-MiniRtcSource([string]$text, [string]$marker, [string]$replacement) {
    if (!$text.Contains($marker)) {throw "The MiniRTC managed adapter did not match: $marker"}
    return $text.Replace($marker, $replacement)
}

$wsHeader=Get-UpstreamText 'src/ws/ws_client.h'
$wsHeader=Replace-MiniRtcSource $wsHeader '  int Connect(const std::string& uri);' @'
  int Connect(const std::string& uri);
  void OpenExternal(std::function<void(const std::string&)> send);
'@
$wsHeader=Replace-MiniRtcSource $wsHeader '  std::function<void(WsStatus)> on_ws_status_ = nullptr;' @'
  std::function<void(WsStatus)> on_ws_status_ = nullptr;
  std::function<void(const std::string&)> external_send_;
'@
[IO.File]::WriteAllText((Join-Path $source 'src/ws/ws_client.h'), $wsHeader)

$ws=Replace-MiniRtcSource $ws 'void WsClient::Send(const std::string& message) {' @'
void WsClient::OpenExternal(std::function<void(const std::string&)> send) {
  external_send_ = std::move(send);
  ws_status_ = WsStatus::WsOpened;
}

void WsClient::Send(const std::string& message) {
  if (external_send_) {
    if (!shutdown_) external_send_(message);
    return;
  }
'@
[IO.File]::WriteAllText((Join-Path $source 'src/ws/ws_client.cpp'), $ws)

$pcHeader=Get-UpstreamText 'src/pc/peer_connection.h'
$pcHeader=Replace-MiniRtcSource $pcHeader '  int Init(PeerConnectionParams params);' @'
  int Init(PeerConnectionParams params,
           std::function<void(const std::string&)> external_send = {});
  int ReceiveExternalSignal(const std::string& signal);
'@
$pcHeader=Replace-MiniRtcSource $pcHeader '  bool offer_peer_ = false;' @'
  bool offer_peer_ = false;
  bool external_signaling_ = false;
'@
[IO.File]::WriteAllText((Join-Path $source 'src/pc/peer_connection.h'), $pcHeader)

$pc=Get-UpstreamText 'src/pc/peer_connection.cpp'
$pc=Replace-MiniRtcSource $pc 'int PeerConnection::Init(PeerConnectionParams params) {' @'
int PeerConnection::Init(PeerConnectionParams params,
                         std::function<void(const std::string&)> external_send) {
'@
$pc=Replace-MiniRtcSource $pc '  const auto address = ParseSignalServerAddress(cfg_signal_server_ip_);' @'
  external_signaling_ = static_cast<bool>(external_send);
  if (!external_signaling_) {
  const auto address = ParseSignalServerAddress(cfg_signal_server_ip_);
'@
$pc=Replace-MiniRtcSource $pc '  uri_ = address->Url(signal_server_port_);' @'
  uri_ = address->Url(signal_server_port_);
  }
'@
$pc=Replace-MiniRtcSource $pc "  if (ws_transport_) {`n    ws_transport_->Connect(uri_);`n  }" @'
  if (external_signaling_) {
    ws_transport_->OpenExternal(std::move(external_send));
    ws_status_ = WsStatus::WsOpened;
  } else {
    ws_transport_->Connect(uri_);
  }
'@
$pc=Replace-MiniRtcSource $pc "  inited_ = true;`n  return 0;`n}" @'
  inited_ = true;
  if (external_signaling_) {
    // The application has already authenticated and authorized this room.
    ProcessSignal(json{{"type", "login"}, {"status", "success"},
                       {"user_id", user_id_}, {"ice_config_version", 1}}.dump());
  }
  return 0;
}

int PeerConnection::ReceiveExternalSignal(const std::string& signal) {
  if (!inited_ || !external_signaling_) return -1;
  try {
    ProcessSignal(signal);
    return 0;
  } catch (const nlohmann::json::exception&) {
    LOG_WARN("Invalid external connection signaling message");
    return -1;
  }
}
'@
[IO.File]::WriteAllText((Join-Path $source 'src/pc/peer_connection.cpp'), $pc)

$apiHeader=Get-UpstreamText 'src/api/minirtc.h'
$apiHeader=Replace-MiniRtcSource $apiHeader 'typedef struct MiniRtcNetTrafficStats {' @'
struct MiniRtcVideoSendQueueStats {
  uint64_t packets = 0;
  uint64_t total_wait_us = 0;
  uint32_t p95_bucket_ms = 0;  // 1 ms buckets; the final bucket includes 100 ms and above.
};

typedef struct MiniRtcNetTrafficStats {
'@
$apiHeader=Replace-MiniRtcSource $apiHeader '} MiniRtcNetTrafficStats;' @'
  // Selected ICE protocol. Relay reports do not infer the TURN control transport.
  enum class Path : uint8_t { Unknown, HostUdp, HostTcp, DirectUdp, DirectTcp, Relay };
  Path selected_path = Path::Unknown;
  MiniRtcVideoSendQueueStats video_send_queue{};
} MiniRtcNetTrafficStats;
'@
[IO.File]::WriteAllText((Join-Path $source 'src/api/minirtc.h'), $apiHeader)

$transportHeader=Get-UpstreamText 'src/transport/ice_transport.h'
$transportHeader=Replace-MiniRtcSource $transportHeader '  TraversalType traversal_type_ = TraversalType::TP2P;' @'
  std::atomic<TraversalType> traversal_type_{TraversalType::TP2P};
  std::atomic<MiniRtcNetTrafficStats::Path> selected_path_{MiniRtcNetTrafficStats::Path::Unknown};
'@
[IO.File]::WriteAllText((Join-Path $source 'src/transport/ice_transport.h'), $transportHeader)

$transport=Get-UpstreamText 'src/transport/ice_transport.cpp'
$transport=Replace-MiniRtcSource $transport '          minirtc_net_traffic_stats.rtt_ms = rtt_ms.value_or(-1);' @'
          minirtc_net_traffic_stats.rtt_ms = rtt_ms.value_or(-1);
          minirtc_net_traffic_stats.selected_path = selected_path_.load();
          if (ice_transport_controller_) {
            minirtc_net_traffic_stats.video_send_queue =
                ice_transport_controller_->TakeVideoSendQueueStats();
          }
'@
$transport=Replace-MiniRtcSource $transport "  MiniRtcNetTrafficStats net_traffic_stats{};`n  net_traffic_stats.srtp_active" @'
  using Path = MiniRtcNetTrafficStats::Path;
  const bool udp = local->transport == NICE_CANDIDATE_TRANSPORT_UDP;
  const bool host = local->type == NICE_CANDIDATE_TYPE_HOST &&
                    remote->type == NICE_CANDIDATE_TYPE_HOST;
  const auto path = traversal_type_ == TraversalType::TRelay ? Path::Relay :
      host ? (udp ? Path::HostUdp : Path::HostTcp) : (udp ? Path::DirectUdp : Path::DirectTcp);
  selected_path_ = path;
  MiniRtcNetTrafficStats net_traffic_stats{};
  net_traffic_stats.selected_path = path;
  net_traffic_stats.srtp_active
'@
$transport=$transport.Replace('TraversalMode(traversal_type_)', 'TraversalMode(traversal_type_.load())')
[IO.File]::WriteAllText((Join-Path $source 'src/transport/ice_transport.cpp'), $transport)
