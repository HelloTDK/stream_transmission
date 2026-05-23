# 弱网高丢包图传设计说明

## 目标

本项目面向远距离、高干扰、高丢包图传场景，优先级为：

```text
图像完整性 > 传输稳定性 > 画质清晰度
```

网络较差时允许分辨率、帧率、色彩和清晰度下降，但不应黑屏、不应连接断开、不应持续花屏。

## 推荐技术栈

使用 GStreamer 作为媒体管线框架，WebRTC 作为实时传输协议。GStreamer 更适合做动态媒体管线、编码器替换、RTP/RTCP 反馈和 Linux 硬件编码适配；FFmpeg 更适合作为测试、录制、离线转码和辅助工具。

## 发送端管线

当前示例管线：

```text
video source
 -> videoconvert
 -> videoscale
 -> videorate
 -> capsfilter
 -> H.264 encoder
 -> h264parse
 -> rtph264pay
 -> webrtcbin
```

其中 `capsfilter` 用于动态切换分辨率和帧率，编码器 `bitrate` 属性用于动态切换码率。

## 接收端管线

接收端通过 `webrtcbin` 接收 RTP 视频流，动态创建解码链路：

```text
webrtcbin
 -> rtph264depay
 -> h264parse
 -> avdec_h264
 -> videoconvert
 -> autovideosink
```

实际部署时可把 `avdec_h264` 替换为硬件解码器。

## 自适应策略

控制器根据丢包率、RTT、jitter 和可用带宽估算网络状态。
各档位阈值和编码输出已在 `config/default.yaml` 中配置化，核心目标是：

```text
图像完整性 > 传输稳定性 > 画质清晰度
```

| 状态 | 丢包率 | 典型输出 |
|---|---:|---|
| Good | < 5% | 1080p/720p，高帧率，高码率 |
| Medium | 5%-20% | 中高码率，轻微降级 |
| Bad | 20%-45% | 480p，15-20fps |
| Severe | 45%-80% | 360p，8-12fps |
| Extreme | >= 80% | 240p/160p，1-5fps，保底图像 |

降级要快，恢复要慢：

- 降级窗口建议 200-500ms。
- 升级窗口建议 3-10s。
- 避免网络抖动时频繁在两个档位之间来回切换。

当前默认配置：

- 码率上限：2Mbps，对应 `video.max_bitrate_kbps: 2000`。
- 码率下限：80kbps，对应 `video.min_bitrate_kbps: 80`。
- 80% 以上丢包进入 `Extreme` 档，默认 320x240、5fps、120kbps 上限。
- 估算带宽可用时只使用 70%，对应 `network.bandwidth_safety_ratio: 0.70`，避免把弱链路打满。
- RTT 和 jitter 也参与分档，避免只看丢包导致高延迟场景响应不足。

## 极端丢包保底策略

80% 以上丢包时，主视频流可能无法稳定承载连续视频。推荐在后续版本中加入独立保底图像通道：

```text
主视频流：尽量保持实时连续
保底图像流：低频发送完整低清图像
```

保底图像可使用：

- 160p/240p/360p
- 1-5fps
- JPEG/WebP/H.264 IDR 帧
- 强 FEC 或重复发送
- CRC 校验，接收端只显示完整图像

这样即使主视频因为极高丢包无法稳定解码，接收端仍能周期性收到完整可识别图像。

当前仓库已有 `GuardFrameChannel` 的分包和 CRC 完整性校验骨架，也已在配置中增加 `guard_stream` 参数。但保底图像通道的实际传输承载尚未接入，后续需要选择以下一种方式落地：

- WebRTC DataChannel：复用 WebRTC 建连和 NAT 穿透，适合先快速闭环。
- 独立 UDP + FEC：更适合强控制远距离链路，但需要自研拥塞控制和重传策略。
- QUIC/自研可靠链路：适合需要加密、多路复用和更细粒度重传控制的版本。

在 80%-90% 丢包要求下，不能只依赖 H.264 RTP 重传。必须配合极低码率、低帧率、短关键帧间隔和保底完整帧通道，否则“不中断、不黑屏、帧完整解码”的验收目标很难稳定达成。

## 当前实现状态

已实现：

- GStreamer/WebRTC 发送端和接收端基础管线。
- H.264 编码、RTP payload、WebRTC 传输。
- 自适应档位控制，支持根据丢包率、RTT、jitter、估算带宽降码率/降分辨率/降帧率。
- 最大码率、最小码率、升降级窗口、丢包阈值、RTT/jitter 阈值、五档编码输出配置化。
- 接收端通过 `webrtcbin get-stats` 尝试提取 packetsLost、packetsReceived、RTT、jitter、bytesReceived，并回传给发送端驱动自适应。
- 接收端 `rtpjitterbuffer` 延迟和过期帧丢弃配置。
- STUN 和 TURN 配置入口。
- 保底图像通道分包/CRC 校验骨架。

尚需落地：

- 在目标 Linux/GStreamer 版本上校准 `get-stats` 字段名和单位，确认丢包率、RTT、jitter、带宽估算曲线准确。
- WebSocket/MQTT/其他真实信令服务器，替换控制台 JSON 手动信令。
- 保底图像通道实际传输、接收、重组和显示逻辑。
- FEC/RED/ULPFEC 或应用层重复发送策略，用于 80% 以上丢包测试。
- 端到端延迟测量和码率曲线记录工具。

## 后续增强点

1. 接入真实 WebSocket 信令服务器。
2. 在接收端统计真实 RTP 丢包率并回传发送端。
3. 添加 ULPFEC/RED 或外部 FEC 通道。
4. 引入 SVC/Simulcast，让低清层优先传输。
5. 对关键帧、SPS/PPS、保底图像包设置更高优先级。
6. 增加自动化弱网测试脚本和测试报告生成。
