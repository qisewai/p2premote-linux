# P2P Remote

当前实现覆盖[设计文档](windows-remote-desktop-design.md)中的实验 A，并提供实验 B 的 WebRTC 依赖验证和单机双进程串流。`core` 负责采集、渲染、WebRTC 会话和统计，`host` 和 `viewer` 提供 Win32 操作界面。

## 一体化客户端与公网账号服务

`p2premote_qt.exe`（Qt）和 `p2premote.exe`（原生 Windows 界面）均可作为主控和被控端。客户端默认填写 `https://111.170.34.243:8443`，也可选择自己的账号服务。登录后，选中账号下的在线设备并点击「连接设备」即可远控。Qt 客户端在「本机」打开「允许远程控制本机」后可配置局域网接收，登录并添加本机后可启用云端接收。原生 Windows 界面仍通过「开始接收」接入。选中设备并点击「移除设备」可撤销该设备的接收资格，正在进行的入站会话也会结束。

武汉入口使用应用内配置的服务器公钥校验 HTTPS 和 WSS。续签时沿用服务端公钥和签发 CA，更换公钥或 CA 时需要重新构建并更新客户端。

Windows Qt 客户端默认在退出或被强制结束时断开入站会话，服务进入休眠。打开「无人值守」后，客户端退出仍可接收连接。关闭被控总开关会卸载服务。移动或更新客户端后，若「本机」提示「更新被控服务」，点击更新可保留现有通道和凭据。退出账号会先关闭云端接收，取消管理员授权会保留登录状态。

在 Windows Qt 客户端登录已绑定的账号，并开启 AI Agent 诊断权限后，云端接收可交给当前桌面客户端逐次显示审批。此时云端会话使用客户端进程的权限。客户端退出后，后台服务按无人值守设置决定是否继续接收，后台不批准诊断请求。

两端安装内置 `clipboard-sync` 插件后，已授权的远控会话支持双向复制粘贴文本。连接时会先同步主控端现有的文本，随后同步两端新复制的文本，每条最多 60 KiB。可将程序旁 `plugins/clipboard-sync/plugin.json` 的 `enabled` 改为 `false`，下次连接时关闭同步。插件接口见 [Lua 插件设计](lua-plugin-design.md)。

两端更新客户端后，可用 `Ctrl+C` 复制文件或文件夹，在另一端的文件管理器中按 `Ctrl+V` 粘贴。两端均采用虚拟文件按需传输，仅在粘贴时读取文件内容。一次复制最多 512 项，含子文件和子目录，清单上限为 60 KiB。请保持会话连接和源文件可读。若要关闭文件剪贴板，把程序旁 `plugins/file-clipboard/plugin.json` 的 `enabled` 改为 `false` 并重新连接。

Qt 主控的「输入」菜单提供快捷键、文本模板和键盘状态同步，「工具」菜单提供文件传输和远程终端，「画面」菜单提供智能画质和浮层开关。设置中的「输入与控制」「画面与音频」「画面浮层」提供对应配置。键盘同步和智能画质首次使用默认关闭。智能画质在「自动」档切换码率，分辨率独立设置。快捷输入和键盘同步需两端更新客户端。

远控窗口顶部可分别选择 720p、1080p 或 1440p 的传输分辨率，以及省流、自动或清晰画质。默认使用 1080p、自动。三个画质档的视频码率预算上限分别为 2、8、16 Mbps，画面菜单显示被控端确认的值。浮层显示解码分辨率和 RTP 收发带宽，包含音频、数据、重传、填充和 SRTP 开销。会话两端都需要支持当前控制协议。

选择 720p 时，MiniRTC 主控端对收到的画面做轻量亮度边缘增强，让放大的文字更易辨认。源桌面缩小时损失的细节取决于原始字号和画面尺寸。

公网会话和 TURN 中继都支持本地光标。用新版客户端连接并接管键鼠后，主控端本地绘制光标位置，被控端同步光标形状。窗口底部显示当前的光标模式。

Viewer 的「连接状态」浮层默认位于画面右下角，显示会话时长、连接类型、呈现帧率、分辨率、上下行带宽、RTT、视频丢包率和帧等待。Win32 工具栏的「浮层」复选框可切换显示。

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

Host 和 Viewer 在局域网整桌面控制中会协商本地光标。Host 单独传输指针形状和热点，Viewer 接管后在鼠标当前位置显示。Windows 采集源无法分离指针时，指针随视频显示。

用 `Start-Viewer-SyncedCursor.cmd` 或给局域网 Viewer 加 `--video-cursor`，可以对照画面同步光标。Windows 将 Host 光标与桌面合到同一条视频流里，拖拽时 Viewer 看到两者一起更新。这个模式下光标响应也受视频回显延迟影响。

局域网直连的 Viewer 使用 0–30 ms 的 WebRTC 播放目标范围。局域网直连和选中 UDP 直连的 MiniRTC 会话都使用 DXGI 同步间隔 0。`--test-playout-delay` 可独立将 WebRTC 播放延迟目标设为 0 ms。其他模式可用 `--test-present-immediate` 验证 DXGI 同步间隔 0。

局域网 Host 的 WebRTC 屏幕共享使用 6 Mbps 的初始带宽估计、更高的短时发包速率和约 30 ms 的发送队列排空目标。编码码率上限由画质档位决定，拥塞控制仍根据网络反馈调整发送预算。

直连默认使用 60 fps。若要换显示器或切回 30 fps，在 Host 先点「断开」，调整选项后点击「等待直连」。键盘布局和输入法由被控机处理。Windows 的权限隔离限制向更高权限的应用或 UAC 安全桌面发送键鼠输入。[Windows SendInput](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-sendinput)

### H.264 硬件模式

在两端的启动参数中添加 `--hardware-video`。Host 使用 Media Foundation 的 H.264 硬件编码器及 D3D11 缩放和色彩转换，Viewer 使用 DXVA 解码，并在 GPU 上把解码纹理转换为显示格式。状态栏显示实际协商的编解码器，并标记硬件和 GPU 转换是否启用。双方未共同提供 H.264 硬件模式时协商 VP8。局域网测试包提供 `Start-Viewer-Hardware.cmd` 以及用于对照的 `Start-Viewer.cmd`。

Viewer 状态栏显示「直写上屏」时，GPU 在一次视频处理操作里完成色彩转换和缩放，直接写入交换链。驱动不支持时使用原有的 GPU 转换和绘制路径。

Host 使用 DXGI Desktop Duplication 采集桌面。

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

持续滚动的回归测试可运行 `out\build\windows-x64-qt\idle_scroll_test.exe --continuous`。它以 1080p、60 fps 和 8 Mbps 视频预算在本机传输滚动的文字与图像，预热后记录 8 秒。输出中的 `max_submit_gap_ms` 是发送端最长提交间隔，`max_update_gap_ms` 是主控端取得的相邻解码画面之间的最长间隔。该测试要求至少取得 440 帧且最长更新间隔小于 100 ms，同时比对两端的 RTP 码率统计。若发送机自身未能按时提交，会单独报告计时无效。

运行 `idle_scroll_test.exe --encoder-backlog` 可检查编码积压时的拖动回显。测试短暂阻塞 GPU，同时提交多个位置不同的画面，释放 GPU 后验证主控能收到最后一个位置。

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

账号服务使用当前 `signaling` 源码构建，沿用现有数据库、已登记设备、HTTPS 和 TURN 配置。手动设备管理使用 `/api/v2`，需要一起升级账号服务和客户端。两端使用 MiniRTC 建立会话，符合配置时同时尝试公网直连和 TURN 中继。可用 `-stun-urls stun:103.236.93.69:23969` 指定西安 STUN。

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

在安装目录运行 `p2premote_qt.exe`。选择「记住密码并自动登录」后，成功登录的凭据按服务地址写入 Windows 凭据管理器。下次启动时，客户端用上次成功登录的服务地址和已保存的凭据自动登录。主动「退出账号」会暂停自动登录，重新手动登录并保存凭据后恢复。主窗口关闭可收起到系统托盘，托盘菜单可停止接收或退出程序。远控时 F11 切换全屏，鼠标移到窗口顶边可打开工具栏。点击画面接管键鼠后，按 F12 释放。

启用系统服务时，安装程序会把被控端组件和加密的账号配置部署到 `Program Files\P2PRemoteHost`。旧版系统服务需要先卸载，再从新版客户端启用并重新绑定。文件传输、文件剪贴板和远程终端适用于登录用户进程中的普通接收模式。

远控画面获得键鼠焦点时，主控端暂时使用直接键盘输入。按 F12、切换标签页或离开画面会恢复原输入法。

在 Qt 远控窗口的「画面 → 画面浮层」可以分别开关各插件，也可以一键隐藏全部。选择会保存，关闭总开关后保留各插件的选择。「连接状态」默认显示在右下角，「会话秒表」默认隐藏，启用后显示在右上角。字号、位置和不透明度可以在设置中心的对应插件页调整，保存后立即应用。

便携目录的 `licenses/` 包含 vcpkg 提供的 Qt 与其依赖许可。Qt Base 的对应源码归档可在 [Qt 下载站](https://download.qt.io/archive/qt/6.11/6.11.3/submodules/) 获取，构建补丁来自 [vcpkg qtbase port](https://github.com/microsoft/vcpkg/tree/277978ad2eddc3b05ecf288dcd33071968cd2f79/ports/qtbase)。

## GitHub 发版与客户端更新

[发版工作流](.github/workflows/release.yml) 在推送与 `VERSION` 一致的 `vX.Y.Z` 标签时构建 Windows 和 Linux x64。两端测试通过后，发布任务签名并校验清单、整包和可用的增量包，随后把完整的草稿 Release 发布为最新版本。在 Actions 手动运行工作流可验证构建、签名和草稿上传。临时草稿会自动清理，签名后的验证产物可在对应运行中下载。

工作流缓存 Windows 的 vcpkg 二进制包以及两端的 MiniRTC 库和头文件。发版配置或依赖文件推送到 `main` 时会运行构建和测试，为后续版本标签预热缓存。也可以在 `main` 手动运行来预热。工具链或依赖变化造成缓存缺失时，工作流会重新编译对应依赖。

首次发版前，把 `p2prelease keygen` 生成的公钥保存到 `cmake/update-public.key`，并把对应私钥的 Base64 内容存为仓库的 Actions secret `P2PREMOTE_UPDATE_PRIVATE_KEY`。发布新版本时先更新 `VERSION` 和 `CHANGELOG.md`，再给该提交打标签并推送标签。普通的分支推送可以继续用于提交代码。

桌面客户端在缓存中有与当前版本和签名匹配的整包时，会优先下载从该版本生成的 BSDIFF40 `.delta` 文件。客户端验签后在缓存中还原并复验整包，随后按正常流程提示重启。首次升级、底包缺失、没有更小的增量包或增量校验失败时，客户端会下载整包。Linux 无人值守被控服务在空闲时使用整包更新。

### Linux 客户端

Linux x86-64 主控端与 Windows 共享 Qt 主窗口、设置、会话标签和控制中心。可在「局域网直连」中选择发现的被控端，或输入地址、端口与验证码。登录账号后可在「我的设备」中连接在线设备。点击远程画面接管键鼠，F11 切换全屏，F12 释放键鼠。支持播放远端声音和从当前会话发送麦克风声音。

两端使用相同的 Lua 脚本和插件协议。Linux 支持文本剪贴板、文件剪贴板、文件传输窗口、远程终端、快捷宏、键盘锁定状态、自动码率、连接状态、会话秒表和只读诊断。账号和局域网凭据使用桌面的 Secret Service 密钥环，已保存的账号可在客户端启动时自动登录。

Linux 作为被控端时，局域网和云端连接均支持双向文本和文件剪贴板。桌面辅助进程以当前登录用户身份运行，只在已授权会话连接到已解锁的本地 X11 桌面时接入剪贴板。安装新版后，重新打开 Linux 客户端或重新登录桌面即可启动辅助进程。锁屏、注销或切换到另一用户的桌面会断开剪贴板共享，返回桌面后重新连接远控会话即可恢复。

Linux 文件管理器和 Windows 资源管理器可以双向复制粘贴文件或文件夹。两侧均通过虚拟文件在粘贴时按需读取内容：Linux 接收时通过用户态 FUSE3 挂载只读虚拟目录（需要 `/dev/fuse` 与 `fusermount3`），复制时不提前下载文件内容，也不占用本地临时目录空间。

Linux 主控端开始绘制时取最新的解码帧。连接状态的 FPS 统计完成窗口缓冲交换的新视频帧，CSV 同时记录解码帧数、解码到绘制的等待时间和绘制到缓冲交换的耗时。端到端回显仍需在实际连接中测量。

Linux 被控端默认以最高 30 fps 检查桌面变化。局域网和云端会话在画面变化后立即提交新帧，并在变化停止后短暂补帧以恢复画质。桌面持续静止时约每秒提交一次缓存画面，期间跳过重复画面的缩放和转色。

构建需要 GCC、CMake、Ninja、Xmake、Qt 6.8 或更新版本（Widgets、Network、WebSockets）、OpenSSL、libyuv、Lua 5.4、libsecret、PulseAudio 和 FUSE3（`fuse3`）的开发包，以及 [libghostty-vt 1.0.1](https://lib.ghostty.org/vt/installing.html#build-from-source)。Linux 上从源码构建 libghostty-vt，可安装到 `~/.local/share/p2premote/ghostty-vt`，也可在配置时用 `CMAKE_PREFIX_PATH` 指定其前缀。MiniRTC 还需要归档解压工具、Meson 1.4 或更新版本、Autotools 和 NASM。

Ubuntu 上还需安装 `libavcodec-dev libavutil-dev` 来构建 MiniRTC，安装 `zlib1g-dev libbz2-dev` 来构建更新模块，以及安装 `libxcb1-dev libxcb-shm0-dev libxcb-xfixes0-dev libxau-dev libsystemd-dev libpulse-dev` 来构建被控服务。

```bash
bash tools/minirtc_probe/Build-Linux.sh
cmake --preset linux-x64 -DCMAKE_PREFIX_PATH="$HOME/.local/share/p2premote/qt/6.8.3/gcc_64"
cmake --build --preset linux-x64
ctest --preset linux-x64
sudo cmake --install out/build/linux-x64
sudo systemctl daemon-reload
p2premote
```

MiniRTC 默认安装到 `~/.local/share/p2premote/minirtc`。自定义路径时给脚本设置 `P2PREMOTE_LINUX_MINIRTC_DIR`，并把同一路径作为 CMake 的 `-DP2PREMOTE_LINUX_MINIRTC_DIR`。Xmake 可通过 `XMAKE_BIN` 指定。

安装后，在「本机」打开「允许远程控制本机」，通过桌面的 PolicyKit 授权，再按需打开「允许局域网连接」。首次安装默认关闭被控。升级时会保留旧版已启用的通道。Linux 被控支持 Xorg 同屏控制，采集跟随活动的虚拟终端，键鼠通过内核的 `/dev/uinput` 注入。登录界面和用户桌面都应运行 Xorg。

默认情况下，退出或强制结束客户端会断开远程会话，服务进入休眠。关闭窗口到托盘会保留前台进程。虚拟机需要在客户端退出后或登录前继续接收时，可以单独开启「无人值守」。关闭「允许远程控制本机」会停止并移除被控服务。

使用云端被控时，登录账号并点击「添加到云端」。总开关已打开时，添加操作会同时申请启用云端接收。已经绑定的设备可点击「启用云端接收」重试授权。退出账号前会停止本机的云端接收。可通过 `journalctl -u p2premote-host.service` 查看局域网服务日志。

云端服务使用绑定到本机设备的登录会话。用于开机登录的配置保存在 root 专用的 `/etc/p2premote/cloud-host.conf`，关闭云端接收时移除。运行状态和错误可通过 `journalctl -u p2premote-cloud-host.service` 查看。

局域网和云端被控都支持转发系统声音。主控端在会话顶部的「声音设置」中勾选「远端扬声器声音」即可收听。被控端采集当前 Xorg 用户的默认音频输出，支持 PulseAudio 和 PipeWire 的 PulseAudio 兼容服务。登录界面和没有可用音频输出的桌面可继续进行画面与键鼠控制。

只需要主控端时使用原有的 `linux-x64-viewer` 预设。WSL 2 的 NAT 网络可能收不到局域网广播，可以手动填写被控端地址，或用 `ip -4 route show default` 查询本机 Windows 网关地址。

### Linux 安装包（`.tar.gz` 与 `install.sh`）

除了 AppImage，还提供包含自包含运行库与安装脚本 `install.sh` 的系统安装包，统一通过 `sudo` 安装到 `/usr/local`：

- 在线一键安装（自动从 GitHub Release 拉取清单并用内置公钥校验 `Ed25519` 签名后再安装）：

```bash
curl -fsSL https://github.com/qisewai/p2premote-linux/releases/latest/download/install.sh | sudo bash
```

  可选参数：`--version <X.Y.Z>`（安装指定版本）、`--mirror <https://...>`（通过 HTTPS 镜像加速下载，仍强制验签）、`--uninstall`（卸载系统安装与被控服务，保留用户目录 `~/.config/P2PRemote` 中的个人设置）。例如：

```bash
curl -fsSL https://github.com/qisewai/p2premote-linux/releases/latest/download/install.sh | sudo bash -s -- --uninstall
```

- 离线 `.tar.gz` 安装（下载 `p2premote-<版本号>-linux-x64.tar.gz` 后解压执行，或直接指定 `--archive`）：

```bash
mkdir -p p2premote-install
tar -xzf p2premote-<版本号>-linux-x64.tar.gz -C p2premote-install
sudo bash p2premote-install/install.sh
```

首次安装默认关闭被控接收，需在界面「本机 → 允许远程控制本机」启用；升级安装会保留 `/etc/p2premote` 中的现有配置，刷新已部署的 `p2premote-host.service` 单元并重启原本运行中的服务，若任一步骤失败则自动回滚到升级前状态。

在已配置 `linux-x64`（`CMAKE_INSTALL_PREFIX=/usr/local`）的 Linux 构建机上，安装 Python 3、`patchelf` 和 OpenSSL 后即可在本地生成可直接分发的 `.tar.gz` 与 `install.sh`：

```bash
bash tools/p2prelease/Package-LinuxTarball.sh out/build/linux-x64 dist/packages
```

### Linux AppImage

在 Ubuntu 22.04 x86-64 上构建 `linux-x64` 预设，安装前缀用 `/usr/local`。准备 Python 3、`patchelf` 和 [appimagetool](https://github.com/AppImage/appimagetool/releases)，再生成主被控合一的 AppImage：

```bash
bash tools/p2prelease/Package-AppImage.sh out/build/linux-x64 dist/packages/P2PRemote-x86_64.AppImage
```

赋予 AppImage 执行权限后即可直接打开。没有可用的 FUSE 时，可加 `--appimage-extract-and-run` 参数启动。首次开启「允许远程控制本机」时，桌面会请求管理员授权，把客户端和被控服务安装到固定的系统路径，然后重新打开窗口。安装后，普通被控和无人值守继续使用现有的开关。

可以从终端执行 `sudo env TMPDIR=/root ./P2PRemote-x86_64.AppImage --appimage-extract-and-run --install` 安装或更新系统副本。此命令在仅管理员可访问的目录中解包，保留现有被控配置。首次安装仍由用户开启被控。便携运行时通过下载新 AppImage 更新，已安装的副本可继续使用现有签名更新。

## 计时口径

- `source_qpc` 保留 WGC 给出的合成帧时间。`capture_qpc` 记录应用从帧池取到帧的时间，用作应用内计时起点。`capture_ready_qpc` 标记应用拿到帧纹理。`draw_begin_qpc` 到 `submit_qpc` 包含交换链调整、GPU 纹理复制和绘制指令提交的调用耗时。[WGC 帧时间戳文档](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframe.systemrelativetime)
- `present_return_qpc` 是 DXGI `Present` 调用返回时间。屏幕发光延迟需要用高帧率相机测量。测试图的明暗交替带和帧号可用于实验 B 的双屏录像。
- 状态栏显示最近 300 个呈现帧的分段及总耗时 P50/P95。运行中显示最近一次刷新区间的帧率，停止后显示整段会话的平均值。队列深度统计应用持有的待呈现帧，峰值为 1。
- 丢帧计数覆盖应用已收到但未呈现的帧，包括被新帧覆盖、尺寸变化期间跳过和停止时清除的待呈现帧。WGC 内部未交付给应用的帧无法从该计数得知。CSV 保留最近 262,144 个已呈现帧及每帧对应的累计计数。调整窗口触发的缓存画面重绘不计入新帧统计。
