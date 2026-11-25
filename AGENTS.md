# AGENTS.md

## 项目与目录

P2P Remote 是一个 Windows x64 远程桌面项目。Windows 客户端兼具主控与被控功能，支持局域网直连和云端账号连接。先读 [README.md](README.md) 中与任务相关的章节，以当前实现和构建配置为准。

| 目录 | 职责 |
| --- | --- |
| `viewer/qt/` | Qt 6 Widgets 客户端。主窗口在 `account_window.*`，远控窗口在 `remote_window.*`，共用主题和无边框窗口实现。 |
| `viewer/`、`host/` | 原生 Win32 界面、远控画面窗口和 Windows 被控服务。`p2premote_service.exe` 是 Windows 被控服务。 |
| `core/`、`proto/` | 采集、编解码、MiniRTC、会话、输入、插件宿主和协议。Qt 通过 `core/include/p2premote/core_api.h` 中的 C ABI 调用核心。 |
| `signaling/` | 独立的 Go 账号、设备登记、WebSocket 信令和 STUN/TURN 服务。Windows 本地构建可命名为 `p2premote_signaling.exe`。 |
| `plugins/`、`agent/` | 内置 Lua 插件、只读诊断的 CLI 和 MCP 入口。 |
| `cmake/`、`tools/` | 构建、打包、MiniRTC 依赖补丁和诊断探针。 |

涉及设计取舍时，按需查阅 [远程桌面设计](windows-remote-desktop-design.md)、[Lua 插件设计](lua-plugin-design.md) 或 [AI Agent 诊断设计](ai-agent-design.md)。

## 构建与打包

- Windows 构建使用 x64 Visual Studio Developer PowerShell、VS 2022、Windows SDK 10.0.22621.0 或更新版本和 CMake 3.23 或更新版本。Qt 预设还需要 Ninja 和 README 指定的 libghostty-vt。将 `VCPKG_ROOT` 指向已完成初始化的 vcpkg 目录。
- MiniRTC 默认从 `out/minirtc/` 读取 `minirtc.h` 和 `p2premote_minirtc.dll`。缺少依赖时按 README 运行 `tools/minirtc_probe/Build.ps1`。非默认依赖目录可用 `P2PREMOTE_MINIRTC_DIR` 和 `P2PREMOTE_GHOSTTY_VT_DIR` 指定。
- 从仓库根目录运行下列命令。已有可用的 Qt 构建目录时可直接执行单目标构建。这个命令把新版可执行文件留在 `out/build/windows-x64-qt/`。

```powershell
cmake --preset windows-x64-qt
cmake --build --preset windows-x64-qt-release --target p2premote_qt
```

- 需要完整构建和安装时，运行 `cmake --build --preset windows-x64-qt-release --parallel`。需要便携 ZIP 时，运行 `./Package-Windows.cmd`，程序安装到 `dist/app/`，ZIP 默认生成在 `dist/packages/`。打包前检查 `dist/app/` 中的程序是否正在运行，避免中断现有会话。
- `out/` 和 `dist/` 是本地依赖和产物目录。修改对应源码或构建入口，避免把临时产物加入版本控制。

## 验证

| 改动 | 建议的验证方式 |
| --- | --- |
| Qt 界面 | 构建 `p2premote_qt`，验证受影响的窗口和状态切换。涉及状态刷新时覆盖一次无变化的轮询和一次真实变化。 |
| C++ 核心、协议、插件或终端 | 先构建相关测试目标，再用 `ctest --preset windows-x64-qt-release -R <测试名>` 运行。测试目标和名称见 [CMakeLists.txt](CMakeLists.txt) 与 [cmake/qt-frontend.cmake](cmake/qt-frontend.cmake)。 |
| 采集、编码和远控延迟 | 按改动检查 `idle_scroll_test`、`continuous_scroll_test`、`encoder_backlog_test` 或 `lan_video_test`。这类验证需要 Windows 和可用的 D3D11 GPU。 |
| Go 账号、信令或中继 | 运行 `go -C signaling test ./...`。需要本机联调时可用 `go -C signaling build -o ../out/p2premote_signaling.exe .`，用回环地址和临时数据库启动。 |

完整 Windows 构建后可运行 `ctest --preset windows-x64-qt-release`。交付时说明实际验证过的路径和因环境限制跳过的检查。

## 修改约定

- 保留工作区已有改动，沿用所在模块的命名和格式。C++ 使用项目现有的 C++20 配置，Go 使用 `gofmt`。Qt 控件复用 `viewer/qt/app_theme.*` 和现有组件。
- 「本机」是独立入口，局域网接收无需云端登录。「我的设备」展示登录账号下已绑定的设备，添加本机由用户主动触发。修改账号或设备流程时，检查登录、刷新、移除与退出后的状态。
- Qt 界面更新留在 UI 线程，沿用异步账号和会话接口。轮询结果未变时，避免重建控件或重复应用样式。
- 修改共享 C ABI、账号 `/api/v2`、信令或插件协议时，同步检查两端调用方和旧数据。保留设备绑定、被控授权、凭据存储与只读诊断权限边界。
- 本地验证使用测试账号和临时数据库。密码、令牌、私钥、生产数据库和含敏感信息的诊断日志放在仓库之外。部署按已指定的目标和范围执行，保留持久数据库及现有 TLS/TURN 配置，更新后验证服务和 `/healthz`。
