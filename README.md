# P2P Remote

当前实现覆盖[设计文档](windows-remote-desktop-design.md)中的实验 A，并提供实验 B 的 WebRTC 依赖验证和单机双进程串流。`core` 负责采集、渲染、WebRTC 会话和统计，`host` 和 `viewer` 提供 Win32 操作界面。

## 一体化客户端与公网账号服务

`p2premote.exe` 同时包含主控和被控能力。客户端默认填写 `https://111.170.34.243:8443`，可在「服务地址」中改用其他服务。两台电脑使用同一服务，第一台注册账号，后续设备登录该账号。每台电脑会自动登记名称和平台，在线后出现在「我的设备」。选中另一台可连接的电脑并点「连接设备」即可远控。本机可以同时接收远控和连接其他设备。

武汉入口使用应用内配置的服务器公钥校验 HTTPS 和 WSS。续签时沿用服务端公钥和签发 CA，更换公钥或 CA 时需要重新构建并更新客户端。

登录后，本机在当前 Windows 桌面会话中接收同账号设备的控制请求。「停止接收」可让本机下线，同时保留主控能力。「退出登录」会关闭本机的远控会话并撤销本次登录。启动电脑后需要登录 Windows 并打开客户端。

两端安装内置 `clipboard-sync` 插件后，已授权的远控会话支持双向复制粘贴文本。连接时会先同步主控端现有的文本，随后同步两端新复制的文本，每条最多 60 KiB。可将程序旁 `plugins/clipboard-sync/plugin.json` 的 `enabled` 改为 `false`，下次连接时关闭同步。插件接口见 [Lua 插件设计](lua-plugin-design.md)。

两端更新客户端后，可在 Windows 资源管理器中用 `Ctrl+C` 复制文件或文件夹，在另一端的目标目录按 `Ctrl+V` 粘贴。复制时只同步文件清单，粘贴时按需读取内容。一次复制最多 512 项，含子文件和子目录，清单上限为 60 KiB。开始粘贴前请保持会话连接和源文件可读。若要关闭文件剪贴板，把程序旁 `plugins/file-clipboard/plugin.json` 的 `enabled` 改为 `false` 并重新连接。

主控工具栏的「插件」菜单提供快捷键、输入文本模板、键盘状态同步和智能画质。键盘同步与智能画质首次使用时默认关闭，开关会在下次连接时恢复。智能画质只在「自动」画质下切换码率，分辨率独立设置。可在主控的 `%LOCALAPPDATA%\P2PRemote\plugin-config\shortcut-macros.viewer.json` 中设置文本模板，例如 `{"template":"您好，已收到。"}`。快捷输入和键盘同步需两端更新客户端。

远控窗口顶部可分别选择 720p、1080p 或 1440p 的传输分辨率，以及省流、自动或清晰画质。默认使用 1080p、自动。三个画质档分别将视频码率上限设为 4、8、16 Mbps，实际码率随网络和画面变化调整。传输尺寸受被控屏幕大小约束。状态区显示实际收到的尺寸、带宽及被控端确认的码率上限。与旧客户端连接时，窗口会显示兼容的分辨率和码率组合档。

选择 720p 时，MiniRTC 主控端对收到的画面做轻量亮度边缘增强，让放大的文字更易辨认。源桌面缩小时损失的细节取决于原始字号和画面尺寸。

公网会话和 TURN 中继都支持本地光标。用新版客户端连接并接管键鼠后，主控端本地绘制光标位置，被控端同步光标形状。窗口底部显示当前的光标模式。

Viewer 会在视频右上角显示诊断浮层，包含会话时长、连接类型、呈现帧率、分辨率、上下行带宽、RTT、视频丢包率和缓冲时间。取消勾选工具栏的「诊断」可隐藏浮层。

公网 NAT 直连和 TURN 中继都启用动态帧率。画面变化时立即提交新帧，静止时约每秒刷新一次。断开后可导出 CSV，查看 ICE 统计中双方的公网映射候选、UDP 直连探测请求和回应的采样峰值。CSV 不包含候选地址。

排查公网直连时，两台电脑都用诊断包里的 `Start-ICE-Diagnostics.cmd` 启动客户端，再连接一次并保持约 20 秒。脚本会在程序旁创建 `ice-logs`。每次建连最多记录首分钟的 ICE 候选地址、端口、路径选择和逐对探测收发计数。主控端日志为 `viewer-ice-*.log`，被控端为 `host-ice-*.log`。请私下分享日志中的网络地址。从源码构建时，可将 `tools/Start-ICE-Diagnostics.cmd` 复制到 `p2premote.exe` 旁使用，也可运行 `p2premote.exe --ice-trace "日志目录"`。

日志中的 `event=ice_connection`、`event=peer_connection` 和 `event=selected_pair` 记录 ICE 状态、整体连接状态和选路变化的时间。两端可用 `p2premote.exe --stun stun:103.236.93.69:23969 --stun-only --ice-trace ice-logs` 对照仅直连模式。该模式用于诊断，连接失败时会直接显示错误。日常启动会保留服务配置的 TURN 兜底。

两端都用 `p2premote.exe --relay-only` 启动并连线，可强制通过账号服务提供的 TURN 中继。

账号和设备保存在服务端的数据库文件中。同一 Windows 用户再次登录会沿用本机的设备标识。服务使用 HTTPS/WSS，TURN 使用与登录会话绑定的临时凭据。WebRTC 按 ICE 候选建立连接，可直连时优先直连，也可通过同一服务的 TURN 转发加密视频与键鼠数据。Viewer 面板显示「直连」或「TURN 中继」。

### 部署 Go 服务

Linux x64 服务器使用打包的 `p2premote-service-linux-amd64`。从源码构建可运行 `go -C signaling build -o /tmp/p2premote-service .`。下面以 HTTPS 8443 端口为例，将证书和域名换成自己的，`PUBLIC_IPV4` 填服务器对外可达的 IPv4 地址。

```sh
chmod +x ./p2premote-service-linux-amd64
PUBLIC_IPV4='203.0.113.10'
./p2premote-service-linux-amd64 \
  -bind 0.0.0.0 -port 8443 -managed-db ./accounts.db \
  -tls-cert /path/to/fullchain.pem -tls-key /path/to/privkey.pem \
  -turn-ip "$PUBLIC_IPV4" -turn-port 3478 -relay-min 40000 -relay-max 40199
```

服务器需要开放 TCP 8443、TCP/UDP 3478 和 UDP 40000–40199。云服务器在使用公网 IPv4 映射时，这些端口都要能到达服务进程。Windows 客户端的服务地址填写证书对应的域名，如 `https://remote.example.com:8443`。数据库 `accounts.db` 应保存在持久磁盘上。

使用 Nginx 提供标准 HTTPS 443 时，可给同一个 Go 服务增加 `-proxy-socket /run/p2premote-web/nginx.sock -proxy-gid <Nginx 的组 ID>`。让 Nginx 通过该 Unix socket 转发 HTTP 和 `/signal` 的 WebSocket，并将 `X-P2PRemote-Client-IP` 覆盖为 Nginx 看到的客户端地址。客户端填写 `https://remote.example.com`。限制 socket 目录和组权限，并为 STUN/TURN 保留各自的监听端口。

如果 Windows 客户端登录超时，可以用 `p2premote.exe --compat-account` 启动兼容登录。兼容模式通过 libcurl 和 Windows 证书库处理账号请求，服务地址仍填写 HTTPS 根地址。

### 本机运行

在 Windows 解压目录中运行以下命令，然后启动同目录的 `p2premote.exe`，服务地址填写 `http://127.0.0.1:8788`。

```powershell
.\p2premote_service.exe -bind 127.0.0.1 -port 8788 -managed-db .\accounts.db -turn-ip 127.0.0.1
```

本机模式的账号接口只监听回环地址。Windows WebRTC 使用网卡地址发起 TURN 连接。单机测试中继时，将 `-turn-bind` 和 `-turn-ip` 都设为本机同一个局域网 IPv4，转发目标仍限于该地址。跨机器测试使用 HTTPS 部署方式。

## 构建与运行

目标运行环境为 Windows 11 x64 的 SDR 桌面，显卡需支持 D3D11。构建需要 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载、Windows SDK 10.0.22621.0 或更新版本，以及 CMake 3.23 或更新版本。Win32 预设无需 vcpkg 依赖，Qt 6 客户端使用独立预设。

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

Viewer 可在连接中分别调整传输分辨率和视频码率上限，默认使用 1080p 和自动画质。状态栏按秒显示 WebRTC 会话上下行带宽。

Viewer 的连接状态面板显示实际选中的 ICE 路径和 UDP/TCP 协议、RTT、近期视频丢包率、编解码方式和收流、解码、呈现帧率。没有有效分母的视频丢包样本显示为「—」。导出的统计 CSV 同时包含协议和累计视频包计数。

已建立的局域网直连遇到信令或视频中断时，Viewer 会在约 30 秒内自动重试，Host 也会恢复监听。在 Host 使用确认模式时，重连仍需要确认。任一端主动结束会话、明确拒绝请求，或 Viewer 在重连期间点击「断开」，都会结束本轮重连。恢复后重新点击画面接管键鼠。测试自动重连时使用同一版本的 Host、Viewer 和信令程序。

新版 Host 和 Viewer 在局域网整桌面控制中会协商本地光标。Host 单独传输指针形状和热点，Viewer 接管后在鼠标当前位置显示。旧版对端或无法关闭光标采集的 Windows 仍可使用画面里的光标。

用 `Start-Viewer-SyncedCursor.cmd` 或给局域网 Viewer 加 `--video-cursor`，可以对照画面同步光标。Windows 将 Host 光标与桌面合到同一条视频流里，拖拽时 Viewer 看到两者一起更新。这个模式下光标响应也受视频回显延迟影响。

局域网直连的 Viewer 使用 0–30 ms 的 WebRTC 播放目标范围。局域网直连和选中 UDP 直连的 MiniRTC 会话都使用 DXGI 同步间隔 0。`--test-playout-delay` 可独立将 WebRTC 播放延迟目标设为 0 ms。其他模式可用 `--test-present-immediate` 验证 DXGI 同步间隔 0。

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

### 自动验证静止后滚动首帧

在带 D3D11 GPU 的 Windows 机器上，从仓库根目录运行以下命令。探针加载可执行文件同目录的 MiniRTC DLL，在本机建立 Host 和 Viewer，无需登录或操作桌面。

```powershell
cmake --build --preset windows-x64-qt-release --target idle_scroll_test
ctest --test-dir out/build/windows-x64-qt -R '^idle_scroll_test$' -V
```

测试在 1080p、60 fps 下先确认接收端看到静止图，再等待一秒、补发旧帧，随后只提交一次带新标记的画面。补帧后 2 ms 和 250 ms 两组各跑 5 次。输出记录实际提交间隔和从提交到解码的毫秒数。500 ms 内未解码出新标记会输出 `MISS` 并返回非零退出码。把 `idle_scroll_test.exe` 复制到另一份解压的 Windows 发布包中运行，即可检验那一份 DLL。

持续滚动的回归测试可运行 `out\build\windows-x64-qt\idle_scroll_test.exe --continuous`。它以 1080p、60 fps 和 8 Mbps 上限在本机传输滚动的文字与图像，预热后记录 8 秒。输出中的 `max_submit_gap_ms` 是发送端最长提交间隔，`max_update_gap_ms` 是主控端取得的相邻解码画面之间的最长间隔。该测试要求至少取得 440 帧且最长更新间隔小于 100 ms。若发送机自身未能按时提交，会单独报告计时无效。

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

### 独立 UDP 打洞测试

`p2premote_nat_probe.exe` 可以在两台 Windows 电脑上单独运行。它默认用同一个 IPv4 UDP 套接字访问西安 STUN `103.236.93.69:23969` 和原节点 `179.253.242.112:3478`，列出各节点看到的公网映射，再用同一个套接字进行双向直连探测。测试只依赖 STUN 和两台电脑之间的 UDP。

1. 两端各打开同一新版程序，等待它显示 `udp1.` 开头的连接码。
2. 交换连接码，在两端分别粘贴对方的码并按 Enter，尽量同时开始。默认探测 30 秒。
3. 把两端的 `nat-probe-*.txt` 报告放在一起查看。程序会打印保存路径，Windows 下存放在 `%LOCALAPPDATA%\P2PRemote\NATProbe`。报告含网络地址和端口。

探测先发送独立工具生成的 ICE 格式 STUN 探测，再在同一套接字上加入普通 UDP 对照。报告按格式分别统计发出的探测、收到的请求和匹配的回复。STUN 回复还带回对端看到的本机地址和端口，可以和通告值比较。两端均零接收时，报告仍无法区分 NAT 映射变化、入站过滤等原因。

仅测 STUN 可运行 `p2premote_nat_probe.exe -discover-only`。用 `-stun 103.236.93.69:23969` 只测西安节点，用 `-bind 192.168.0.43:0` 绑定本机指定 IPv4 地址，用 `-duration 60s` 延长探测。使用 Go 1.23 或更新版本，在 `signaling` 目录运行 `go build -o ../out/p2premote_nat_probe.exe ./cmd/natprobe` 即可构建。

### MiniRTC 双端数据回环

实验分支使用 CrossDesk v1.7.0 固定的 MiniRTC `f5b53e920db0713b456e23af6a0fe9907d94a612`。回环程序使用该版本的原生 ICE、DTLS-SRTP 和数据流 API，记录实际直连／中继路径、路径变化、匹配回复数和最低应用往返时间。

从仓库根目录使用 MSVC x64 和 Xmake 3.1.1 构建。`Build.ps1` 会把源码和构建产物放在 `out`。如遇 Xmake 的 Strawberry Perl 安装问题，可以用 `-Perl` 指定便携版 Strawberry Perl 的 `perl.exe`。

```powershell
powershell -ExecutionPolicy Bypass -File tools/minirtc_probe/Build.ps1
Push-Location signaling
go build -o ../out/minirtc-probe/minirtcsignal.exe ./cmd/minirtcsignal
Pop-Location
```

运行 `out\minirtc-probe\minirtcsignal.exe -out out\minirtc-probe`，记录输出中的 `SIGNAL` 和 `STATE_DIR`。信令只监听 `127.0.0.1`，默认向客户端下发西安 STUN。它会生成本次测试的 `host.txt`、`viewer.txt` 和 `ca.pem`。下面用 `47000` 代指 `SIGNAL` 中的端口，用 `SESSION` 代指 `STATE_DIR`。

```powershell
out\minirtc-probe\p2premote_minirtc_probe.exe host wss://127.0.0.1:47000/signal SESSION\host.txt SESSION\ca.pem direct 20
out\minirtc-probe\p2premote_minirtc_probe.exe viewer wss://127.0.0.1:47000/signal SESSION\viewer.txt SESSION\ca.pem direct 20
```

双机时用 SSH 反向端口转发把主控端的本地 WSS 端口接到信令服务。把探测程序、`viewer.txt` 和 `ca.pem` 放到主控机，主控端使用它本机的转发端口。转发承载信令，MiniRTC 在两台电脑间自行建立数据流。程序只在自身进程中加载实验 CA 并校验信令主机名或 IP。

测试自动中继时，给信令增加 `-turn-file <JSON>`，字段为 `urls`、`username`、`credential` 和 Unix 秒 `expires_at`，并把两端的 `direct` 改为 `auto`。可以用 `-stun 103.236.93.69:23969,179.253.242.112:3478` 比较两个 STUN 节点看到的映射。对照 `ROUTE`、`RESULT` 行和 `logs-host`、`logs-viewer` 下的候选对日志。结束后关闭信令并删除两份测试身份文件。分发链接 MiniRTC 的二进制时参照其 [许可和第三方声明](https://github.com/kunkundi/minirtc/blob/f5b53e920db0713b456e23af6a0fe9907d94a612/THIRD_PARTY_NOTICES.md)。

排查公网直连时可以在启动探测程序的 PowerShell 中设置 `$env:P2PREMOTE_MINIRTC_PROBE_ICE_PACING_MS = '60'`，独立调节 libnice 收集候选地址和发送连通性检查的间隔。取值为 20–200 毫秒，默认 20 毫秒。日志中的 `Probe STUN pacing interval` 会确认探测程序采用的值。对照测试时可为每端单独设置，测试完执行 `Remove-Item Env:P2PREMOTE_MINIRTC_PROBE_ICE_PACING_MS` 恢复默认值。

### MiniRTC 桌面和键鼠验证

在两台机器已登录且解锁的 Windows 桌面上运行探测器，并在两端命令末尾追加 `desktop`。被控端启动后共享主显示器，通过 MiniRTC 发送 H.264。主控端窗口收到画面后可操作远端鼠标和键盘。被控端关闭共享提示窗口即可结束会话，主控端可按 F12 或关闭窗口。自动测试可以在两端统一使用 `desktop-check`。它在被控桌面显示专用色块，主控端在看到初始色块后发送鼠标移动、点击和 F8，读取实际显示的画面，确认点击和按键引起的两次颜色变化。

验收看两端的 `DESKTOP=pass` 和 `RESULT=pass`。自动测试要求 `mouse_verified=1` 和 `keyboard_verified=1`。公网直连还要求 `path=direct`，加密要求 `srtp=1`。桌面模式使用实验信令和被控端的主显示器，适用于明确授权的测试会话。

画质回归时，两端把末尾参数改为 `quality-check`，时长建议至少 90 秒。探测器按 60 帧目标配置发送端，以约 50 帧每秒提交 1080p 动态测试图，主控端检查预热后的实际分辨率和解码后的细线对比度。通过时输出 `QUALITY=pass`。

### 账号模式使用 MiniRTC

使用 x64 Visual Studio 开发终端构建库和正式客户端。把 `p2premote.exe`、`p2premote_minirtc.dll`、`plugins/` 及 `lua-notice.html` 一起放到两台电脑，并保持相对路径。

```powershell
powershell -ExecutionPolicy Bypass -File tools/minirtc_probe/Build.ps1
cmake --preset windows-x64-webrtc -DP2PREMOTE_MINIRTC_DIR=out/minirtc
cmake --build --preset windows-x64-webrtc-release --target p2premote
```

账号服务使用当前 `signaling` 源码构建，沿用现有数据库、HTTPS 和 TURN 配置。服务端在同账号设备通过授权后协商媒体后端。带 MiniRTC 模块的两端会优先使用 MiniRTC，符合配置时同时尝试公网直连和 TURN 中继。旧客户端、旧服务和需要 TLS TURN 的配置继续使用 WebRTC。可用 `-stun-urls stun:103.236.93.69:23969` 指定西安 STUN。

从设备列表发起连接后，状态区显示 `H264 (MiniRTC)` 和实际选中的路径。诊断模式 `--ice-trace <目录>` 会把 MiniRTC 的候选对和握手记录写到该目录的 `minirtc.log`。

MiniRTC 主控端直接上传解码后的 NV12 两个平面，由 D3D11 执行 BT.709 转色。使用该路径时状态区显示「GPU 转换」。显卡无法初始化 NV12 纹理或着色器时，客户端使用原有的 CPU 转色路径。

用 `p2premote.exe --latency-trace <目录>` 启动两端，正常断开后会写出延迟 CSV。按输入序号对齐移动、左键和滚轮事件。被控端按帧号记录采集及提交，主控端按本机帧标识对齐 `minirtc_frame_assembled`、`minirtc_decoder_complete`、`minirtc_frame_queued` 与 `frame_present_begin`、`frame_present_submitted`。这些时间戳适合计算各自主机内的耗时。跨机输入时间需对齐时钟，端到端回显也可用变色测试图在主控端计时。

`minirtc_send_queue_interval` 统计该报告周期内成功发送的普通视频 RTP 包。`id` 为样本数，`aux` 为累计队列等待微秒数，`flags` 为第 95 百分位的毫秒分桶。最高桶包含 100 ms 及以上。`frame_present_called` 和 `frame_present_submitted` 分别记录 DXGI `Present` 调用前和返回时的时间，与面板实际显示分开测量。

### Qt 6 客户端

Qt Widgets 客户端使用 `p2premote_core.dll` 的 [C ABI](core/include/p2premote/core_api.h) 访问账号、会话、D3D11 视频和 Lua 插件。两者在 Windows x64 上分别使用动态与静态 MSVC 运行库。Qt 界面通过 Windows 等待句柄驱动采集和呈现。Win32 客户端 `p2premote.exe` 仍可单独构建和部署。

在 x64 Visual Studio 开发终端中设置 `VCPKG_ROOT`，指向已完成 `bootstrap-vcpkg.bat` 的 vcpkg 仓库。以下预设通过 vcpkg 的 `qt-ui` 特性安装 Qt 6，再构建和安装两个客户端。当前验证使用 Qt 6.11.3 和 vcpkg 提交 `277978ad2eddc3b05ecf288dcd33071968cd2f79`。

Qt 远程终端需要 [libghostty-vt 1.0.1 Windows x64 预编译包](https://release.files.ghostty.org/libghostty-vt/1.0.1/ghostty-vt-1.0.1-x86_64-windows-gnu.zip)。按 [Ghostty 官方说明](https://lib.ghostty.org/vt/installing.html#windows) 验证后解压到 `out/ghostty-vt`，让 `out/ghostty-vt/ghostty-vt-1.0.1-x86_64-windows-gnu` 直接包含 `bin/`、`include/` 和 `lib/`。如果解压到其他位置，在首次配置时加上 `-DP2PREMOTE_GHOSTTY_VT_DIR="C:/path/to/ghostty-vt-1.0.1-x86_64-windows-gnu"`。

Windows 发布产物统一放在仓库根目录的 `dist/` 下。

- `dist/app/` 存放最新 Release 程序、运行库和插件。直接运行其中的 `p2premote_qt.exe`。
- `dist/packages/` 存放带时间戳的便携 ZIP。把 ZIP 拷到目标电脑，完整解压后运行 `p2premote_qt.exe`。

准备好构建依赖后，双击仓库根目录的 [Package-Windows.cmd](Package-Windows.cmd) 即可更新程序并生成 ZIP。依赖放在自定义目录时，可用 `.\Package-Windows.cmd -VcpkgRoot <路径> -MiniRtcDir <路径> -GhosttyVtDir <路径>`，也可加 `-OutputDirectory <路径>` 指定 ZIP 输出位置。

也可以手动配置、编译和部署：

```powershell
cmake --preset windows-x64-qt
cmake --build --preset windows-x64-qt-release --parallel
ctest --preset windows-x64-qt-release
```

在安装目录运行 `p2premote_qt.exe`。选择「记住密码」后，成功登录的凭据按服务地址写入 Windows 凭据管理器。主窗口关闭可收起到系统托盘，托盘菜单可停止接收或退出程序。远控时 F11 切换全屏，鼠标移到窗口顶边可打开工具栏。点击画面接管键鼠后，按 F12 释放。

启用系统服务时，安装程序会把被控端组件和加密的账号配置部署到 `Program Files\P2PRemoteHost`。旧版系统服务需要先卸载，再从新版客户端启用并重新绑定。文件传输、文件剪贴板和远程终端适用于登录用户进程中的普通接收模式。

远控画面右下角默认显示连接信息。↓ 和 ↑ 是连接实际的下行和上行速率，fps 是主控端呈现帧率，「帧等待」是解码完成到开始绘制的平均等待时间。点击顶部性能按钮或「画面 → 显示右下角连接信息」可切换显示。拖动时文字模糊，可在「画面 → 画质码率」中切换「清晰 · 16 Mbps」，观察实际下行速率和画面变化。

便携目录的 `licenses/` 包含 vcpkg 提供的 Qt 与其依赖许可。Qt Base 的对应源码归档可在 [Qt 下载站](https://download.qt.io/archive/qt/6.11/6.11.3/submodules/) 获取，构建补丁来自 [vcpkg qtbase port](https://github.com/microsoft/vcpkg/tree/277978ad2eddc3b05ecf288dcd33071968cd2f79/ports/qtbase)。

## 计时口径

- `source_qpc` 保留 WGC 给出的合成帧时间。`capture_qpc` 记录应用从帧池取到帧的时间，用作应用内计时起点。`capture_ready_qpc` 标记应用拿到帧纹理。`draw_begin_qpc` 到 `submit_qpc` 包含交换链调整、GPU 纹理复制和绘制指令提交的调用耗时。[WGC 帧时间戳文档](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime)
- `present_return_qpc` 是 DXGI `Present` 调用返回时间。屏幕发光延迟需要用高帧率相机测量。测试图的明暗交替带和帧号可用于实验 B 的双屏录像。
- 状态栏显示最近 300 个呈现帧的分段及总耗时 P50/P95。运行中显示最近一次刷新区间的帧率，停止后显示整段会话的平均值。队列深度统计应用持有的待呈现帧，峰值为 1。
- 丢帧计数覆盖应用已收到但未呈现的帧，包括被新帧覆盖、尺寸变化期间跳过和停止时清除的待呈现帧。WGC 内部未交付给应用的帧无法从该计数得知。CSV 保留最近 262,144 个已呈现帧及每帧对应的累计计数。调整窗口触发的缓存画面重绘不计入新帧统计。
