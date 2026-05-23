# 弱网高丢包远距离图传项目

本项目是面向 **远距离、高干扰、高丢包图像传输** 场景的 C++17 / GStreamer / WebRTC 工程骨架。设计目标是：在网络质量很差时优先保证图像不中断、可识别；在网络质量较好时尽可能利用带宽提升画质。

> 当前仓库按 Linux 部署环境设计。

## 技术路线

- 主媒体框架：GStreamer
- 实时传输：WebRTC / RTP / RTCP
- 默认编码：H.264，示例使用 `x264enc`
- 弱网策略：码率、分辨率、帧率动态降级
- 抗丢包策略：WebRTC NACK/PLI/FIR/RTCP 反馈 + 编码器关键帧保护 + 保底图像通道预留
- 信令方式：默认使用本机 TCP JSON 行信令，也支持控制台手动 JSON 行信令；后续可替换为 WebSocket/MQTT/自研链路

## 目录结构

```text
.
├── CMakeLists.txt
├── README.md
├── config/
│   └── default.yaml
├── docs/
│   ├── test-cases.md
│   └── weak-network-design.md
└── src/
    ├── adaptive/          # 弱网自适应码率控制器
    ├── app/               # 应用入口编排
    ├── config/            # 配置加载
    ├── media/             # WebRTC 发送端/接收端
    ├── signaling/         # TCP/控制台 JSON 行信令
    └── util/              # 日志和 JSON 辅助
```

## Linux 依赖

Ubuntu/Debian 可参考：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libjson-glib-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  pkg-config \
  libglib2.0-dev \
  libjson-glib-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-nice
```

如果使用硬件编码，需要根据平台额外安装对应插件，例如：

- Intel/VAAPI：`gstreamer1.0-vaapi`
- NVIDIA：对应版本的 GStreamer NVENC 插件
- V4L2 硬编：确认系统内有 `v4l2h264enc`

## 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## 运行方式

当前版本内置两种 JSON 行信令方式：

- 默认：`config/default.yaml` 中 `signaling.url: tcp://127.0.0.1:9000`，接收端监听 TCP，发送端连接 TCP。
- 回退：把 `signaling.url` 留空后，使用控制台 stdin/stdout 手动复制 JSON 行。

默认配置下应先启动接收端，再启动发送端。

接收端：

```bash
./build/weaknet_webrtc --mode recv --config config/default.yaml
```

发送端：

```bash
./build/weaknet_webrtc --mode send --config config/default.yaml
```

命令行参数：

```text
--mode send|recv       运行发送端或接收端
--config <path>        配置文件路径，默认 config/default.yaml
--help                 显示帮助
```

### Python 接收端（兼容当前 C++ 发送端）

仓库提供了 Python 版接收端脚本：`scripts/python_receiver.py`。  
它使用 TCP JSON 行信令，协议字段与当前 C++ 版本一致（`sdpBase64` / `ice` / `metrics`）。

1. 安装 Python 依赖：

```bash
python3 -m pip install aiortc
# 如果需要 --preview / --visualize 实时预览：
python3 -m pip install opencv-python
```

2. 启动 Python 接收端（默认监听 `127.0.0.1:9000`）：

```bash
python3 scripts/python_receiver.py --host 127.0.0.1 --port 9000
```

3. 另开终端启动 C++ 发送端：

```bash
./build/weaknet_webrtc --mode send --config config/default.yaml
```

可选参数：

- `--record out.mp4`：把接收到的视频保存为文件；不传时默认丢弃媒体数据（用于纯链路/自适应联调）。
- `--preview` / `--visualize`：开启 OpenCV 实时画面预览，按 `q` 或 `Esc` 关闭预览窗口；首次使用需要安装 `opencv-python`。
- `--metrics-interval 1.0`：回传网络 metrics 的周期（秒），默认 1 秒。

## 本机 localhost 自收发测试

这一节用于先在同一台 Linux 机器上验证“发送端产生视频，接收端能显示画面”。当前默认配置使用 `uridecodebin` 读取本地文件：

```yaml
video:
  source: uridecodebin uri=file:///home/cjj/streaming_transmission/1.mp4
```

如果你的机器上没有这个文件，先把 `video.source` 改成自己的本地视频文件，或改回测试源：

```yaml
video:
  source: videotestsrc is-live=true pattern=ball
```

### 1. 安装依赖

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libjson-glib-dev \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav
```

建议先确认关键插件存在：

```bash
gst-inspect-1.0 webrtcbin
gst-inspect-1.0 x264enc
gst-inspect-1.0 avdec_h264
gst-inspect-1.0 autovideosink
```

### 2. 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 3. 打开两个终端

终端 A 运行接收端：

```bash
./build/weaknet_webrtc --mode recv --config config/default.yaml
```

终端 B 运行发送端：

```bash
./build/weaknet_webrtc --mode send --config config/default.yaml
```

### 4. 自动 TCP 信令

默认配置下不需要手动复制 offer、answer、ICE 或 metrics。接收端会监听 `127.0.0.1:9000`，发送端会自动连接；连接建立后，信令消息和接收端 metrics 都会通过这个 TCP 连接传递。

接收端日志中应能看到：

```text
TCP 信令监听中: 127.0.0.1:9000
TCP 信令已连接。
```

发送端日志中应能看到：

```text
TCP 信令已连接: 127.0.0.1:9000
已发送 offer。
```

### 5. 手动控制台信令

如果需要手动复制 JSON 行，把配置里的 `signaling.url` 改为空：

```yaml
signaling:
  url:
```

然后启动接收端和发送端。日志是 `[INFO]` / `[WARN]` / `[ERROR]`，不要复制日志，只复制以 `{` 开头、以 `}` 结尾的整行 JSON。

基本顺序：

1. 从发送端终端 B 复制 `{"type":"sdp","sdpType":"offer",...}` 整行，粘贴到接收端终端 A，然后回车。
2. 从接收端终端 A 复制 `{"type":"sdp","sdpType":"answer",...}` 整行，粘贴到发送端终端 B，然后回车。
3. 两边后续可能继续输出 `{"type":"ice",...}`，把发送端的 ICE 复制到接收端，把接收端的 ICE 复制到发送端。
4. 协商成功后，接收端应弹出视频窗口。

连接建立后，接收端会周期性输出 `{"type":"metrics",...}`。这些 metrics 也需要复制给发送端，发送端才会根据丢包率、RTT、jitter 和带宽估算做自适应调节。

### 6. 本机测试配置建议

本机自收发时可以使用默认 TCP 信令配置：

```yaml
app:
  stun_server: stun://stun.l.google.com:19302
  turn_server:

signaling:
  url: tcp://127.0.0.1:9000

video:
  source: uridecodebin uri=file:///home/cjj/streaming_transmission/1.mp4
  max_bitrate_kbps: 2000
```

同一台机器或同一局域网测试通常不依赖 TURN。`stun_server` 只用于 WebRTC NAT 穿透辅助，不是视频服务器地址。

如果只想验证链路、不依赖本地视频文件，可以把 `video.source` 改成：

```yaml
video:
  source: videotestsrc is-live=true pattern=ball
```

### 7. 常见问题

如果没有视频窗口，先检查：

- `gst-inspect-1.0 webrtcbin` 是否存在；没有则安装 `gstreamer1.0-plugins-bad`。
- `gst-inspect-1.0 x264enc` 是否存在；没有则安装 `gstreamer1.0-plugins-ugly`。
- `gst-inspect-1.0 avdec_h264` 是否存在；没有则安装 `gstreamer1.0-libav`。
- `video.source` 指向的本地文件是否存在；如果不确定，先改用 `videotestsrc is-live=true pattern=ball`。
- 默认 TCP 信令下，是否先启动了接收端，且端口 `127.0.0.1:9000` 未被占用。
- 手动控制台信令下，是否把 offer、answer、ice JSON 复制到了对端终端，而不是复制了 `[INFO]` 日志。
- 是否运行在有桌面显示环境的 Linux 终端；无桌面环境下 `autovideosink` 可能无法弹窗。

如果只想确认发送端管线能启动，可以先看发送端日志里是否有：

```text
发送端管线已启动
已发送 offer
```

如果只想确认接收端已等待信令，可以看接收端日志里是否有：

```text
接收端已启动，等待 TCP 信令连接。
```

## 弱网控制逻辑

自适应控制器位于 `src/adaptive/AdaptiveController.*`，当前分为五档：

| 状态 | 丢包率范围 | 策略 |
|---|---:|---|
| Good | < 5% | 接近最大码率，最高画质 |
| Medium | 5%-20% | 轻微降码率，保持连续性 |
| Bad | 20%-45% | 降分辨率/帧率，增强恢复能力 |
| Severe | 45%-80% | 低分辨率、低帧率、保守码率 |
| Extreme | >= 80% | 进入极端保底策略，优先完整图像 |

发送端会把控制器输出应用到编码器和 capsfilter：

- 动态调整 `bitrate`
- 动态调整输出宽高
- 动态调整帧率
- 缩短关键帧间隔

`config/default.yaml` 已把弱网策略参数配置化，重点包括：

- `video.max_bitrate_kbps`：最大码率上限，默认 2000kbps。
- `network.*_loss_threshold`：各网络档位的丢包率阈值，默认 80% 以上进入 Extreme。
- `network.*_rtt_ms` / `network.*_jitter_ms`：高延迟和高抖动触发降级。
- `profiles.good` 到 `profiles.extreme`：每个档位的分辨率、帧率、码率上限和关键帧间隔。
- `receiver.jitter_buffer_latency_ms`：接收端 RTP 抖动缓冲时间。
- `guard_stream.*`：80% 以上极端丢包保底图像通道预留参数。

当前接收端会通过 `webrtcbin get-stats` 尝试回传丢包率、RTT、jitter 和接收带宽，发送端据此做自适应调整。不同 GStreamer 版本的 stats 字段可能有差异，Linux 联调时需要用实际日志校准字段名和单位。

## 后续落地建议

第一阶段建议先在 Linux 上完成：

1. 使用本地视频文件或 `videotestsrc` 验证发送/接收链路。
2. 替换为真实摄像头，例如 `v4l2src device=/dev/video0`。
3. 根据硬件平台替换编码器，例如 `v4l2h264enc`、`vaapih264enc`、`nvh264enc`。
4. 将本机 TCP 信令替换为真实 WebSocket/MQTT/自研信令服务器。
5. 使用 `tc netem` 或 Clumsy 对照 `docs/test-cases.md` 做弱网测试。

## 当前版本边界

当前代码是可扩展工程骨架，核心链路、配置、自适应状态机和 WebRTC 管线已经放好；内置 TCP/控制台信令用于早期联调，不建议直接用于生产。生产版本建议替换 `src/signaling/ConsoleSignalingClient.*`，保留 `SignalingMessage` 数据结构即可。

## 注意事项

80%-90% 丢包属于极端场景，不能只依赖重传。此时必须降低到极低码率，并通过低分辨率、低帧率、关键帧保护、保底图像通道来保证“可识别图像”优先于“清晰流畅”。
