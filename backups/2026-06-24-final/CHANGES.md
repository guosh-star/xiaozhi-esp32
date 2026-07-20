# 2026-06-24 最终变更清单

## Git 已提交 (xiaozhi-esp32, commit f60771f)

| 文件 | 改动 |
|------|------|
| `main/boards/cuckoo-clock/cuckoo_controller.cc` | AI说话时暂停TCP下载 (释放WiFi空口给UDP) |
| `main/protocols/mqtt_protocol.cc` | MQTT断开→Idle (修复永久卡死) + 重连允许背景音频 |
| `main/protocols/mqtt_protocol.h` | 重连间隔 60s→20s |
| `main/audio/audio_service.cc` | 重采样复杂度 2→3 (消除24000→16000发闷失真) |

## Git 未跟踪 (需手动备份)

| 文件 | 改动 |
|------|------|
| `sdkconfig` | WiFi动态RX缓冲 6→16 (核心修复: 消除UDP丢包) |
| `sdkconfig` | UDP mbox 6→20 |
| `sdkconfig` | 唤醒阈值 15→8 (更灵敏) |
| `managed_components/.../esp_udp.cc` | UDP任务优先级 1→5 + 诊断日志 |

## 云端服务器 (120.55.47.160)

| 文件 | 改动 |
|------|------|
| `/opt/qqmusic-proxy/server_async.py` | 异步化 (aiohttp), 支持多设备并发 |

## 本地工作区

| 文件 | 改动 |
|------|------|
| `memory/2026-06-24.md` | 全天记录 |
| `MEMORY.md` | 移除量化交易 |
| `backups/2026-06-24-final/` | 全部7个改动文件的备份 |
