# 2026-07-28 修改记录

## 备份文件
- cuckoo_controller.cc (222193 bytes)
- cuckoo_controller.h (18320 bytes)
- audio_service.cc (43986 bytes)
- audio_service.h (10035 bytes)
- audio_codec.h (1962 bytes)
- sdkconfig (109054 bytes)

## 修改清单与状态

### P0 系统修复
1. ✅ is_running_ — 已确认是 atomic<bool>（之前已修复）
2. ✅ dog_outro_done_ 改为 std::atomic<bool>（cuckoo_controller.h）
3. ✅ PlayPcm 错误路径恢复 SetOutputMuted(false)（3处：client init fail / open fail / HTTP status != 200）

### P1 系统修复
4. ✅ serial fallback fread 阻塞 → select() 1秒超时（cuckoo_controller.cc）
5. ✅ HTTP 重试等待时间 50ms → 200ms（2处：首批下载 + 后续批次下载）

### P2 系统修复
6. ✅ rtc_crash_log 死代码完全移除（结构体定义 + 启动检查 + 30秒心跳写入，共3处代码块）
7. ✅ NVS 闹钟持久化加版本号（kAlarmSchemaVersion=1，SaveAlarmsToNvs 写入 + LoadAlarmsFromNvs 检查）

### P3 系统修复
8. ⏭ TODO(#22) 轮询优化 — 暂不实施（标记保留）

### 音频修复
9. ✅ PlayUrl OutputRawPcm → PushRawPcmToPlayback（2处：重采样路径 + 直通路径）
10. ✅ PlayUrl 双重重采样消除：kOutRate 从 24000 改为 16000（直接到 codec 输出采样率）
11. ✅ DecodeSingleFile OutputRawPcm → PushRawPcmToPlayback（本地 MP3 播放路径）
12. ✅ DMA buffer 增大：DESC_NUM 8→10, FRAME_NUM 512→768（总缓冲 8KB→15KB）
13. ✅ PlayUrl 栈 8KB → 12KB
14. ✅ 限幅 30000 硬削波 → 软限幅（3次多项式近似 tanh，5处全部替换）
15. ✅ PushBackgroundAudio 采样率拒绝改为带日志警告（便于调试）

## 涉及文件
| 文件 | 修改内容 |
|------|---------|
| cuckoo_controller.h | dog_outro_done_ → atomic<bool> |
| cuckoo_controller.cc | P0-P2系统修复 + 音频修复（共12处改动） |
| audio_codec.h | DMA buffer 增大 |
| audio_service.cc | PushBackgroundAudio 日志警告 |

## 未修改的 OutputRawPcm 调用（保留，非音乐路径）
- 唤醒提示音（cuckoo_wake_sound）— 系统提示音，需立即播放
- 钟声同步播放（PlayCuckooSoundSync / PlayBellSoundSync）— 报时精确时序
- 报时钟声 PCM（PerformanceTask 中的 bell_pcm）— 报时精确时序
- 狗叫直通（PlayDogBark 无 bg audio 时）— 短促音效
- DogShow 狗叫 WAV（PlayWavAsset）— 短促音效
- 闹钟铃声（AlarmTask）— 系统提示音
