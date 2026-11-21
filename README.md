# P2P Remote

当前实现覆盖[设计文档](windows-remote-desktop-design.md)中的实验 A，并提供实验 B 的 WebRTC 依赖验证和单机双进程串流。`core` 负责采集、渲染、WebRTC 会话和统计，`host` 和 `viewer` 提供 Win32 操作界面。

## 构建与运行

目标运行环境为 Windows 11 x64 的 SDR 桌面，显卡需支持 D3D11。构建需要 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载、Windows SDK 10.0.22621.0 或更新版本，以及 CMake 3.23 或更新版本。实验 A 仅依赖 Windows SDK，`vcpkg.json` 暂无待安装的包。

在 Visual Studio Developer PowerShell 中构建并启动预览。

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
& .\out\build\windows-x64\Release\p2premote_host.exe
```

在下拉框中选择一个显示器并点「开始预览」。点「停止」结束采集，随后可导出本次预览的逐帧 CSV。预览窗口和采集源位于同一显示器时，画面会显示预览窗口自身。有第二台显示器时，可把窗口放到另一屏观察。

## 实验 A 验证

在被采集的显示器上运行 `p2premote_test_pattern.exe`。测试图包含 RGB 色块、递增帧号、明暗交替带和移动方块。

1. 让 1920 × 1080 的 SDR 显示器播放测试图，开启预览并持续运行 10 分钟。观察状态栏中的采集和呈现帧率，以及丢帧和队列峰值。
2. 检查 RGB 色块顺序、文字方向和画面比例。改变预览窗口宽高比，确认黑边与内容按比例调整。尝试最小化后恢复。
3. 预览期间在系统设置中更改被采集显示器的分辨率，确认预览恢复且状态栏更新到新尺寸。随后恢复原分辨率。
4. 停止并导出 CSV。确认包含各分辨率的帧元数据和连续的 QPC 时间戳。重新启动预览，确认新的会话从头统计。

## 实验 B 依赖验证

验证预设固定使用 Windows x64 的 Shiguredo `m156.8078.3.4`，该发行包使用 MSVC STL 和静态 CRT。版本、WebRTC 源提交、下载地址和 SHA-256 记录在 [`cmake/webrtc-release.cmake`](cmake/webrtc-release.cmake)。下载约 782 MB，校验通过后解压到 `out/deps`。[发行记录](https://github.com/shiguredo-webrtc-build/webrtc-build/releases/tag/m156.8078.3.4)

此预设使用 Ninja 和 MSVC，以 x64 Release 和 `/MT` 构建客户端与验证程序。已验证的工具链为 MSVC 14.44（Visual Studio 2022 17.14）和 Windows SDK 10.0.22621.0。本机原有的 MSVC 14.39 缺少该发行包链接所需的 STL 符号。

在配置好 x64 工具链、CMake 和 Ninja 的 Developer PowerShell 中，从仓库根目录运行以下命令。下载使用 Windows 的 `curl.exe`。

```powershell
cmake -P tools/fetch_webrtc.cmake
cmake --preset windows-x64-webrtc
cmake --build --preset windows-x64-webrtc-release --parallel
ctest --preset windows-x64-webrtc-release -V
```

验证程序用自定义工厂调用内置 VP8，对 1280 × 720 合成帧做一次编码和解码，检查 `kNative` 的 `ToI420()` 回退以及解码像素。随后创建 `PeerConnection`，配置接收端最小抖动缓冲延迟为 0，生成只接收 VP8 视频的 SDP 并关闭连接。两机 LAN 传输和延迟有独立的实验 B 验收项。

## 实验 B 单机双进程串流

先用 `windows-x64-webrtc-release` 预设构建客户端。在仓库根目录使用 Go 1.23 或更新版本构建并启动信令服务。

```powershell
go -C signaling test ./...
go -C signaling build -o ../out/p2premote_signaling.exe .
& .\out\p2premote_signaling.exe
```

信令服务监听 `127.0.0.1:8787`，用 WebSocket 转发会话请求和 SDP。另开终端启动 Host 和 Viewer。

```powershell
& .\out\build\windows-x64-webrtc\p2premote_host.exe --share
& .\out\build\windows-x64-webrtc\p2premote_viewer.exe
```

1. 在 Host 选择显示器并点击「创建会话」。在 Viewer 输入 Host 显示的 12 位十六进制会话代码，再点击「连接」。代码有效期为 2 分钟。
2. 在 Host 点击「允许共享」。WebRTC 连接建立后，Host 开始采集并按采集时间戳节流，按源画面比例缩放到最高 1280 × 720、30 fps。Viewer 开始呈现 VP8 画面。
3. 任一端点击「断开」都会结束会话。可在两端分别导出每秒统计 CSV，然后在 Host 创建新会话重连。

状态栏显示提交、编码、接收、解码和呈现速率。CSV 的帧数是各进程各自从连通开始的累计值，计时项是本机均值。Host 的采集至提交和 Viewer 的绘制至 Present 返回都包含各自的画面转换开销。软件路径还包括 Host 的 GPU 回读和 Viewer 的 D3D11 上传。

### 同机键鼠体验

1. 将 Host 和 Viewer 并排放在同一屏幕上，确保 Host 的测试区可见。按上面的流程连接，在 Host 点击「共享并允许键鼠测试」。
2. 在 **Viewer 回传画面**里点击 Host 的蓝色测试区，观察颜色翻转。按住左键拖动方块，或滚动鼠标、输入文字，观察回显。
3. 按 `Esc` 或切换窗口退出键鼠输入，再次点击画面即可恢复。任一端点击「断开」可以结束会话。

键鼠测试模式将输入送入 Host 测试区，测试区的变化经屏幕采集和串流回到 Viewer。双机整桌面控制在局域网直连模式中启用。

### 720p60 低延迟模式

在 Host 创建会话前勾选「低延迟 60 fps」，可将采集提交与 WebRTC 发送的帧率上限从 30 提高到 60。分辨率上限是 1280 × 720。断开后可切回 30 fps。

两端可在断开后导出 CSV。发送排队是 RTP 视频包从打包到交给网络套接字的平均等待，接收缓冲是视频帧在 WebRTC 抖动缓冲中的平均等待，解码后等待是最终呈现的帧从交给 Viewer 到被取出绘制的平均等待。三项均为本次会话的累计平均值，缺少样本时列为空。Viewer 还记录 WebRTC 丢弃帧数和被更新的解码帧覆盖的帧数。

## 双机局域网测试

把 `p2premote_signaling.exe` 和 `p2premote_host.exe` 放在被控机，将 `p2premote_viewer.exe` 放在操控机。下例用 `192.168.0.43` 代表被控机的局域网 IPv4 地址，请按被控机的 `ipconfig` 输出替换。两台机器都使用被控机的同一个信令地址。

在被控机的程序目录运行：

```powershell
Start-Process .\p2premote_signaling.exe -ArgumentList '-bind 192.168.0.43'
Start-Process .\p2premote_host.exe -ArgumentList '--share --signal ws://192.168.0.43:8787/signal'
```

在操控机的程序目录运行：

```powershell
Start-Process .\p2premote_viewer.exe -ArgumentList '--signal ws://192.168.0.43:8787/signal'
```

在 Host 选择要共享的显示器，可勾选「低延迟 60 fps」，然后创建会话。把 Host 的 12 位代码输入 Viewer 并连接，再回到 Host 点击「允许共享」或「共享并允许键鼠测试」。后者允许 Viewer 在回传画面里点击、拖动和向 Host 测试区输入文字。

这组命令使用明文 WebSocket 信令，适用于可信局域网。视频和键鼠由 WebRTC 直接在两台机器间传输。

- 如果 Viewer 无法连接信令，从操控机运行 `Test-NetConnection 192.168.0.43 -Port 8787`。检查被控机上的信令服务是否在运行，以及防火墙是否允许专用网络访问该程序的 TCP 8787 端口。
- 如果能看到 Host 的授权请求，但授权后视频建连超时，检查两台机器之间的 UDP 连通性，以及 Windows 防火墙是否允许 Host 和 Viewer 使用专用网络。[Windows 防火墙规则说明](https://learn.microsoft.com/en-us/windows/security/operating-system-security/network-security/windows-firewall/configure)

### 一键直连与整桌面控制

信令在被控机用 `-direct-peer` 指定主控机的 IPv4 地址。两端传入 `--direct` 后，Host 启动即等待，Viewer 点击「一键直连」直接发出请求。以下用 `192.168.0.43` 作为 Host、`192.168.0.32` 作为 Viewer 示例。

被控机：

```powershell
Start-Process .\p2premote_signaling.exe -ArgumentList '-bind 192.168.0.43 -direct-peer 192.168.0.32'
Start-Process .\p2premote_host.exe -ArgumentList '--share --direct --signal ws://192.168.0.43:8787/signal'
```

主控机：

```powershell
Start-Process .\p2premote_viewer.exe -ArgumentList '--direct --signal ws://192.168.0.43:8787/signal'
```

在 Host 点击「允许整桌面控制」后，点击 Viewer 画面即可操作共享显示器上的普通 Windows 应用。Host 的启动参数加上 `--auto-control` 后，信令放行的局域网主控机可直接连接并控制桌面。Viewer 正常断开后，自动授权的 Host 会恢复等待。Host 可点击「断开」或按 `Ctrl+Alt+F11` 停止接收连接，再点「等待直连」即可恢复。按 Viewer 的 `F12` 可释放本地键鼠。

测试时可以用 `-direct-lan` 替换 `-direct-peer 192.168.0.32`。信令根据 `-bind` 所在网卡的 IPv4 子网放行 Viewer。与 Host 的 `--auto-control` 同时启用时，同网段内的主控机可以直接接管共享桌面。

新版 Viewer 可在连接中切换 720p、1080p 和 1440p，默认 1080p。对应视频码率上限为 4、8、16 Mbps，实际分辨率受被控屏幕尺寸约束。状态栏按秒显示 WebRTC 会话上下行带宽。

新版 Host 和 Viewer 在局域网整桌面控制中会协商本地光标。Host 单独传输指针形状和热点，Viewer 接管后在鼠标当前位置显示。旧版对端或无法关闭光标采集的 Windows 仍可使用画面里的光标。

用 `Start-Viewer-SyncedCursor.cmd` 或给局域网 Viewer 加 `--video-cursor`，可以对照画面同步光标。Windows 将 Host 光标与桌面合到同一条视频流里，拖拽时 Viewer 看到两者一起更新。这个模式下光标响应也受视频回显延迟影响。

局域网直连的 Viewer 使用 0–30 ms 的 WebRTC 播放目标范围，并使用 DXGI 同步间隔 0 呈现。`--test-playout-delay` 可独立将播放延迟目标设为 0 ms。会话码和公网模式可用 `--test-present-immediate` 单独验证 DXGI 同步间隔 0。

局域网 Host 的 WebRTC 屏幕共享使用 6 Mbps 的初始带宽估计、更高的短时发包速率和约 30 ms 的发送队列排空目标。编码码率上限由画质档位决定，拥塞控制仍根据网络反馈调整发送预算。

直连默认使用 60 fps。若要换显示器或切回 30 fps，在 Host 先点「断开」，调整选项后点击「等待直连」。键盘布局和输入法由被控机处理。Windows 的权限隔离限制向更高权限的应用或 UAC 安全桌面发送键鼠输入。[Windows SendInput](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendinput)

### H.264 硬件模式

在两端的启动参数中添加 `--hardware-video`。Host 使用 Media Foundation 的 H.264 硬件编码器及 D3D11 缩放和色彩转换，Viewer 使用 DXVA 解码，并在 GPU 上把解码纹理转换为显示格式。状态栏显示实际协商的编解码器，并标记硬件和 GPU 转换是否启用。双方未共同提供 H.264 硬件模式时协商 VP8。局域网测试包提供 `Start-Viewer-Hardware.cmd` 以及用于对照的 `Start-Viewer.cmd`。

Viewer 状态栏显示「直写上屏」时，GPU 在一次视频处理操作里完成色彩转换和缩放，直接写入交换链。驱动不支持时使用原有的 GPU 转换和绘制路径。

Host 可以用 `--capture-dxgi` 对照 DXGI Desktop Duplication。局域网 Viewer 启用本地光标后，Host 状态栏显示实际使用的采集方式。画面同步光标使用 WGC。DXGI 无法采集所选屏幕或无法分离光标时回退 WGC。[CrossDesk 的 Windows 采集策略](https://github.com/kunkundi/crossdesk/blob/ff369cd023b7b0481ee94a54570fb9793bb24079/apps/desktop/src/platform/windows/screen_capturer/capture_backend_policy.h)

局域网直连会根据采集画面的变化发送视频。Host 在 GPU 上比较相邻采集帧，变化后的首帧立即进入编码，大幅变化后最多再提交 6 张重复帧以细化画质。画面静止时约每秒刷新一次，Viewer 会保留最近一帧。Host 的 `--fixed-cadence` 可恢复原先的提交策略和 H.264 GOP 配置，用于对照。

使用 `--latency-trace` 时，`h264_gop` 记录驱动接受的 GOP 帧数，`h264_encoded` 的位 0 表示实际 IDR、位 1 表示 WebRTC 请求了关键帧。Viewer 的 `h264_decode_input` 与 `frame_present_submitted` 使用同一个 RTP 时间戳，可对齐关键帧和呈现时间。进行静止画质对照时，让全屏静态图覆盖被控端的状态栏、系统时钟和其他会更新的界面。

排查实时性时，给两端加 `--latency-trace <目录>`，或使用测试包的 Diagnostics 启动脚本。正常断开后，CSV 记录本机单调时钟上的输入序号、视频 RTP 序号及大小、带宽估计、编码目标码率、解码和提交呈现事件。两端的输入序号和 RTP 标识可用于匹配，跨机时间仍需先对齐。日志到达容量上限后会统计省略事件数。

编译工作区中的 `codec_probe.exe` 可以单机验证 H.264 编解码、GPU 纹理和 720p 至 1080p 的尺寸切换。探针报告的是媒体转换耗时。键鼠到画面回显的延迟需要用串流连接测试。

## 公网连接测试

在被控机启动仅监听回环地址的认证信令，再通过 HTTPS 隧道把 `/signal` 提供给两端。公网客户端使用相同的 `wss://域名/signal` 地址和各自的角色密钥。Host 仍需确认每次控制请求。

服务器的密钥文件包含 `hostToken` 和 `viewerToken`，分别填写独立随机生成的 32 字节 Base64URL 值，无需填充。运行 `p2premote_signaling.exe -bind 127.0.0.1 -port 18890 -internet-keys signaling-keys.json`，让隧道指向 `http://127.0.0.1:18890`。临时测试可以使用 Cloudflare Quick Tunnel 或 localhost.run，临时域名以启动时输出为准。[Cloudflare Quick Tunnels](https://developers.cloudflare.com/tunnel/get-started/quick-tunnels/) · [localhost.run HTTP Tunnels](https://localhost.run/docs/http-tunnels)

两端分别准备自己的客户端配置。例如 Host 文件可以这样写：

```json
{
  "signalUrl": "wss://your-tunnel.example/signal",
  "accessToken": "<hostToken>",
  "iceTransportPolicy": "all",
  "iceServers": [
    { "urls": ["stun:stun.cloudflare.com:3478"] },
    {
      "urls": ["turn:your-turn.example:3478?transport=udp"],
      "username": "<TURN username>",
      "credential": "<TURN credential>"
    }
  ]
}
```

用实际地址与凭据替换示例。Viewer 的 `accessToken` 使用 `viewerToken`。准备好文件后，在被控机运行 `p2premote_host.exe --share --internet host.json`，在主控机运行 `p2premote_viewer.exe --internet viewer.json`。

公网信令仅交换连接信息。WebRTC 使用 STUN 尝试通过 NAT 直连，在配置了可用 TURN 时可以回退到中继。只打算测试 STUN 时可删除示例中的 TURN 项。配置有效的 TURN 后，可把两端的策略改为 `relay`，强制中继以对比体验。通过状态栏和 CSV 的 `ice_path`、`ice_rtt_ms` 检查实际传输路线和往返时间。客户端文件中的访问密钥和 TURN 凭据只发给对应角色。[WebRTC TURN](https://webrtc.org/getting-started/turn-server)

### 公网点对点模式

两端保留各自的 `signalUrl` 和 `accessToken`，将 `iceTransportPolicy` 设为 `"all"`，`iceServers` 只配置 STUN。例如：

```json
{
  "iceTransportPolicy": "all",
  "iceServers": [
    { "urls": ["stun:stun.cloudflare.com:3478"] }
  ]
}
```

两端通过 ICE 检查能互通的地址。画面和键鼠使用选中的点对点通道，连接失败时界面会报超时。要实测 NAT 打洞，把 Viewer 切到另一个网络再发起连接，例如手机热点。状态栏的「NAT 直连」表示选中了包含 NAT 候选的直连路径。「主机候选直连」在同一局域网测试时通常表示走的是局域网。[ICE 连接检查](https://www.rfc-editor.org/rfc/rfc8445.html) · [Cloudflare STUN 服务](https://developers.cloudflare.com/realtime/turn/)

## 计时口径

- `source_qpc` 保留 WGC 给出的合成帧时间。`capture_qpc` 记录应用从帧池取到帧的时间，用作应用内计时起点。`capture_ready_qpc` 标记应用拿到帧纹理。`draw_begin_qpc` 到 `submit_qpc` 包含交换链调整、GPU 纹理复制和绘制指令提交的调用耗时。[WGC 帧时间戳文档](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime)
- `present_return_qpc` 是 DXGI `Present` 调用返回时间。屏幕发光延迟需要用高帧率相机测量。测试图的明暗交替带和帧号可用于实验 B 的双屏录像。
- 状态栏显示最近 300 个呈现帧的分段及总耗时 P50/P95。运行中显示最近一次刷新区间的帧率，停止后显示整段会话的平均值。队列深度统计应用持有的待呈现帧，峰值为 1。
- 丢帧计数覆盖应用已收到但未呈现的帧，包括被新帧覆盖、尺寸变化期间跳过和停止时清除的待呈现帧。WGC 内部未交付给应用的帧无法从该计数得知。CSV 保留最近 262,144 个已呈现帧及每帧对应的累计计数。调整窗口触发的缓存画面重绘不计入新帧统计。
