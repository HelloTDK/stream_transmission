# Windows 编译和运行流程

本文档用于在 Windows 上编译并运行 `weaknet_webrtc`。

## 1. 安装工具

推荐使用 64 位 MSVC 工具链：

1. 安装 Visual Studio 2022，勾选 `使用 C++ 的桌面开发`。
2. 安装 CMake，或使用 Visual Studio 自带的 CMake。
3. 安装 GStreamer 1.0 MSVC x86_64 的 Runtime 和 Development 两个安装包。

GStreamer 官方下载地址：

- 下载页面：https://gstreamer.freedesktop.org/download/#windows
- Windows 安装包目录：https://gstreamer.freedesktop.org/data/pkg/windows/

本项目推荐下载 `msvc-x86_64` / `msvc_x86_64` 版本，并安装 Runtime 和 Development/Devel 两个 `.msi` 包。文件名通常类似：

```text
gstreamer-1.0-msvc-x86_64-<version>.msi
gstreamer-1.0-devel-msvc-x86_64-<version>.msi
```

GStreamer 安装路径建议使用默认路径：

```text
C:\gstreamer\1.0\msvc_x86_64
```

注意：要安装 MSVC 版本，不要混用 MinGW 版本的 GStreamer。

## 2. 配置环境变量

打开 PowerShell，执行：

```powershell
$env:GSTREAMER_1_0_ROOT_MSVC_X86_64 = "D:\SoftWare\Gstream_msvc_x86_64"
$env:Path = "$env:GSTREAMER_1_0_ROOT_MSVC_X86_64\bin;$env:Path"
$env:PKG_CONFIG_PATH = "$env:GSTREAMER_1_0_ROOT_MSVC_X86_64\lib\pkgconfig"
```

如果你的 GStreamer 安装在其他目录，把上面的路径改成实际路径。

上面的 `$env:... = ...` 只对当前 PowerShell 窗口有效。新开一个 PowerShell 后，这些临时环境变量不会自动存在。

如果希望新开的 PowerShell 也能使用这些环境变量，可以永久写入当前用户环境变量：

```powershell
[Environment]::SetEnvironmentVariable(
  "GSTREAMER_1_0_ROOT_MSVC_X86_64",
  "D:\SoftWare\Gstream_msvc_x86_64",
  "User"
)

[Environment]::SetEnvironmentVariable(
  "PKG_CONFIG_PATH",
  "D:\SoftWare\Gstream_msvc_x86_64\lib\pkgconfig",
  "User"
)
```

还需要把 GStreamer 的 `bin` 目录永久加入当前用户的 `Path`：

```powershell
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$gstBin = "D:\SoftWare\Gstream_msvc_x86_64\bin"


if ($userPath -notlike "*$gstBin*") {
  [Environment]::SetEnvironmentVariable(
    "Path",
    "$gstBin;$userPath",
    "User"
  )
}
```

永久写入后，关闭所有 PowerShell，再重新打开一个新的 PowerShell 验证。

验证当前 PowerShell 会话里是否已经写入：

```powershell
$env:GSTREAMER_1_0_ROOT_MSVC_X86_64
$env:PKG_CONFIG_PATH
Test-Path "$env:GSTREAMER_1_0_ROOT_MSVC_X86_64\lib\pkgconfig"
Get-ChildItem "$env:GSTREAMER_1_0_ROOT_MSVC_X86_64\lib\pkgconfig\gstreamer-1.0.pc"
```

验证 `pkg-config` 是否能找到 GStreamer：

```powershell
Get-Command pkg-config
pkg-config --version
pkg-config --modversion gstreamer-1.0
pkg-config --modversion glib-2.0
pkg-config --modversion json-glib-1.0
pkg-config --cflags --libs gstreamer-1.0
```

如果 `Test-Path` 返回 `False`，通常是没有安装 Development/Devel 包，或 GStreamer 路径写错。

可以先检查插件是否可用：

```powershell
gst-inspect-1.0 webrtcbin
gst-inspect-1.0 x264enc
gst-inspect-1.0 avdec_h264
gst-inspect-1.0 autovideosink
```

这几个命令都能输出插件信息时，再继续编译。

## 3. 生成 Visual Studio 工程

在仓库根目录执行：

```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64
```

如果 CMake 报找不到 GStreamer、glib 或 json-glib，优先检查：

- `PKG_CONFIG_PATH` 是否指向 `...\lib\pkgconfig`
- Runtime 和 Development 包是否都已安装
- 当前终端是否已经重新设置了环境变量

## 4. 编译

```powershell
cmake --build build-win --config Release
```

编译成功后，程序路径通常是：

```text
build-win\Release\weaknet_webrtc.exe
```

## 5. 运行本机测试

Windows 下建议先使用 `config/windows.yaml`，它使用 `videotestsrc` 测试画面，不依赖 Linux 摄像头设备。该配置默认使用 `127.0.0.1:19000` 做本机 TCP 信令，避开一些 Windows 环境中容易被系统组件预留的低位端口。

先启动接收端：

```powershell
.\build-win\Release\weaknet_webrtc.exe --mode recv --config config\windows.yaml
```

再打开另一个 PowerShell，设置同样的 GStreamer 环境变量后启动发送端：

```powershell
.\build-win\Release\weaknet_webrtc.exe --mode send --config config\windows.yaml
```

正常情况下，接收端会监听 `127.0.0.1:19000`，发送端会自动连接 TCP 信令。

## 6. 常见问题

### 找不到 `gst-inspect-1.0`

说明 GStreamer 的 `bin` 目录没有加入 `Path`。重新执行：

```powershell
$env:Path = "C:\gstreamer\1.0\msvc_x86_64\bin;$env:Path"
```

### CMake 找不到 `gstreamer-1.0`

通常是 `PKG_CONFIG_PATH` 没设置，或没有安装 Development 包。

```powershell
$env:PKG_CONFIG_PATH = "C:\gstreamer\1.0\msvc_x86_64\lib\pkgconfig"
```

### 程序启动时报找不到 DLL

运行程序的终端里没有设置 GStreamer `bin` 路径。运行前确认：

```powershell
$env:Path = "C:\gstreamer\1.0\msvc_x86_64\bin;$env:Path"
```

### 默认配置启动失败

`config/default.yaml` 当前偏 Linux 环境，里面可能使用 `v4l2src` 等 Linux 视频源。Windows 先使用：

```powershell
.\build-win\Release\weaknet_webrtc.exe --mode send --config config\windows.yaml
```

后续接真实摄像头时，再把 `video.source` 改成 Windows 可用的 GStreamer 视频源。

### 绑定 TCP 端口失败，WinSock error 10013

`WinSock error 10013` 表示 Windows 拒绝绑定该地址或端口。即使 `netstat` 看不到占用，也可能是端口被 Hyper-V、WSL、Docker 或系统网络组件预留了。

先查看普通端口占用：

```powershell
netstat -ano | findstr :19000
```

再查看 Windows 预留端口范围：

```powershell
netsh interface ipv4 show excludedportrange protocol=tcp
netsh interface ipv6 show excludedportrange protocol=tcp
```

如果当前端口落在 excluded port range 里，换一个不在列表中的端口，例如：

```yaml
signaling:
  url: tcp://127.0.0.1:20000
```

接收端和发送端必须使用同一个配置文件、同一个端口。
