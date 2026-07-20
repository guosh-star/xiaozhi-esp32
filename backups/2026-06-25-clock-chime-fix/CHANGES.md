# 报时修复 - 2026-06-25

## 问题
整点和半点都不报时

## 根因（两处）

### 1. 夜间静音区间覆盖了早上6点
`CheckTime()` 中 `hour < 7` 把 0:00~6:59 全部静音，导致 6:00/6:30 不报时。

### 2. 每日 NTP 同步不设 time_set_ = true
`cuckoo_clock_task` 开机后 15 秒做初始 NTP 同步，此时设 `time_set_ = true`。
如果 WiFi 连接慢、NTP 未就绪，`time_set_` 永远是 false，时钟不走。

每日 NTP 同步 (`tick_sec % 86400`) 更新了时钟值但漏了 `time_set_ = true`，
导致开机 NTP 同步失败后时钟永远不会启动。

## 修复

| # | 文件 | 改动 |
|---|------|------|
| 1 | cuckoo_controller.cc:2055 | `hour < 7` → `hour < 6` |
| 2 | cuckoo_controller.cc:2946 | 每日 NTP 同步补上 `sm->time_set_ = true` |
| 3 | config.h | SERVO_DOG GPIO48 → GPIO33 (J3-17，排针直插) |
| 4 | config.h | BUILTIN_LED_GPIO NC → GPIO48（启用WS2812） |
| 5 | cuckoo_board.cc | GetLed 改用 CircularStrip (WS2812, 1灯) |
| 6 | circular_strip.cc | Speaking 色：绿→蓝 (RGB: 0,0,255) |

## 颜色定义
- 聆听 (Listening): 红色 RGB(255,0,0)
- 说话 (Speaking): 蓝色 RGB(0,0,255)
- 待机 (Idle): 熄灭
- 启动/连接: 蓝色呼吸

| 7 | sdkconfig | 控制台 UART0 → USB Serial/JTAG（脱机不卡死） |

## 文件
- `cuckoo_controller.cc` (2 处改动)
- `config.h` (2 处改动)
- `cuckoo_board.cc` (1 处改动)
- `circular_strip.cc` (1 处改动)
- `sdkconfig` (3 行改动)
