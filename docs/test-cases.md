# 弱网高丢包图传测试用例

## 测试工具

Windows 可使用 Clumsy，Linux 建议使用 `tc netem`。

Linux 示例：

```bash
sudo tc qdisc add dev eth0 root netem loss 20% delay 100ms 20ms
sudo tc qdisc change dev eth0 root netem loss 80% delay 200ms 50ms
sudo tc qdisc del dev eth0 root
```

## TC-01 基础码率控制

- 条件：`video.max_bitrate_kbps` 设置为 2000。
- 方法：测试静态画面、快速运动画面。
- 判定：实时码率峰值不超过 2Mbps，平均码率在 1.5-2Mbps 区间内或随画面复杂度合理降低。
- 观测：使用 `webrtcbin get-stats`、网卡监测或外部抓包工具记录发送码率。

## TC-02 轻度丢包适应性

- 条件：丢包率 5%-35%，对应 `Medium` 到 `Bad` 档。
- 方法：观察接收端画面连续性。
- 判定：画面连续，无持续花屏，可有轻微卡顿但能恢复。
- 配置参考：`network.medium_loss_threshold: 0.05`，`network.bad_loss_threshold: 0.20`。

## TC-03 极高丢包稳定性

- 条件：丢包率 35%-90%，重点观察 45%-90%，对应 `Severe` 到 `Extreme` 档。
- 方法：逐级提升丢包率，观察连接和画面。
- 判定：连接不应断开；不应黑屏；允许低清、低帧率、短时卡顿。
- 配置参考：`network.severe_loss_threshold: 0.45`，`network.extreme_loss_threshold: 0.80`。
- 注意：80% 以上丢包若要求稳定收到完整帧，需要保底图像通道或 FEC/重复发送策略完全接入。当前仓库已有配置和分包校验骨架，传输承载仍需实现。

## TC-04 动态分辨率自适应降级

- 条件：丢包率达到 20% 以上。
- 方法：观察发送端自适应日志和接收端画面。
- 判定：分辨率应从高档平滑降至 480p/360p，界面无长时间黑屏。
- 配置参考：`profiles.bad` 默认 640x480，`profiles.severe` 默认 480x360。

## TC-05 关键帧保障

- 条件：丢包率 5%-15%。
- 方法：观察关键帧丢失后的恢复速度。
- 判定：接收端能在下一次关键帧或 PLI 后恢复完整画面。
- 配置参考：各档 `keyframe_interval` 会随网络变差缩短。

## TC-06 重传与恢复

- 条件：突发丢包或短时断续网络。
- 方法：制造 1-3 秒丢包尖峰。
- 判定：恢复后画面能快速继续显示，连接不重建。
- 注意：WebRTC 可提供 NACK/PLI/FIR 等反馈，但极端丢包下需要限制重传比例，避免重传进一步挤占主链路。

## TC-07 拥塞控制码率自适应

- 条件：丢包率阶梯递增 0% -> 90%。
- 方法：记录码率、分辨率、帧率变化曲线。
- 判定：码率应在 2 秒内明显降低，恢复时无剧烈振荡。
- 配置参考：`network.downshift_window_ms: 500`，`network.upshift_window_ms: 5000`。

## TC-08 图像延时控制

- 条件：不同丢包率下测试端到端延时。
- 方法：使用画面时间戳或 LED/计时器拍屏。
- 判定：常规弱网目标 180-500ms；极端丢包下允许为保图像完整性适当增加。
- 配置参考：`network.target_latency_min_ms`、`network.target_latency_max_ms`、`receiver.jitter_buffer_latency_ms`。

## 推荐测试步骤

1. 先在 0% 丢包下跑通发送端和接收端，确认码率不超过 `video.max_bitrate_kbps`。
2. 按 5%、20%、45%、80%、90% 阶梯增加丢包，观察自适应日志中的档位、码率、分辨率、帧率。
3. 每个丢包档位至少保持 30 秒，避免只观察瞬时恢复。
4. 对 80% 以上丢包单独记录“主视频是否黑屏”和“保底图像是否周期性完整显示”。
5. 网络恢复到 0% 后继续观察 10 秒，确认升级过程不频繁振荡。
