# 布谷鸟钟固件优化记录 2026-05-21

备份文件：
- `cuckoo_controller.cc.bak_20260521_review`
- `cuckoo_board.cc.bak_20260521_review`

---

## 1. LEDC 定时器重复初始化修复
**文件**: `cuckoo_controller.cc`
**问题**: Motor 构造函数每次调用都重新配置 `LEDC_TIMER_0`，Servo 每次重新配置 `LEDC_TIMER_1`。后续调用返回 `ESP_ERR_INVALID_STATE`。
**修复**: 用 `static bool` 标志确保定时器只配置一次。

```cpp
// Motor 构造函数中
static bool motor_timer_inited = false;
if (!motor_timer_inited) {
    ledc_timer_config(&timer_conf);
    motor_timer_inited = true;
}

// Servo 构造函数中
static bool servo_timer_inited = false;
if (!servo_timer_inited) {
    ledc_timer_config(&timer_conf);
    servo_timer_inited = true;
}
```

## 2. 线程安全：is_running_ 改为 atomic

**文件**: `cuckoo_controller.h`
**问题**: `is_running_` 在 Core 0 (MCP) 和 Core 1 (钟控任务) 之间无保护读写，存在数据竞争。
**修复**: 改为 `std::atomic<bool>`，跨核心自动内存屏障。

```cpp
// .h 中
#include <atomic>
std::atomic<bool> is_running_{false};

// IsRunning() 改为
bool IsRunning() { return is_running_.load(); }
```

## 3. DecodeSingleFile 冗余局部变量
**文件**: `cuckoo_controller.cc`
**问题**: 第 246 行 `auto& app = Application::GetInstance();` 在 while 循环外定义但未使用（循环内另行定义）。
**修复**: 移除该行，在函数末尾需要时再获取引用。

## 4. 未使用的 AllocLedcChannel
**文件**: `cuckoo_board.cc`
**问题**: LEDC 通道分配器函数从未被调用（用 `__attribute__((unused))` 抑制警告）。
**修复**: 删除该函数及 `next_ledc_channel` 静态变量，同时移除不再需要的 `#include <driver/ledc.h>`。

## 5. StopAll 简化：移除 Schedule 中的冗余 AbortSpeaking
**文件**: `cuckoo_controller.cc`
**问题**: `StopAll` 外层已同步调用 `AbortSpeaking`，`Schedule` lambda 中又重复调用一次。
**修复**: 移除 lambda 中的 `AbortSpeaking`，保留外层的同步调用（快速打断），lambda 只做状态切换和音频恢复。

## 6. PlayIndex 上限收紧
**文件**: `cuckoo_controller.cc`
**问题**: `PlayIndex` 上限是 13，可直接播 0013.mp3（钟声）。
**修复**: 上限改为 12，与 `PlayBgMusic` 一致，防止误播钟声文件。
```cpp
// 旧: if (index > 13) index = 13;
// 新: if (index > 12) index = 12;
```
