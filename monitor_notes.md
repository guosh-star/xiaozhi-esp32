# ESP32 布谷鸟钟报时监控 — 运行笔记（备份）
监控目标: C:\Users\GUO-notebook\.openclaw\workspace\xiaozhi-esp32\111.txt
监控脚本: chime_monitor.ps1 (后台运行至 2026-08-03 08:00)
事件日志: chime_monitor_report.txt
本文件更新时间: 2026-08-02 22:15

## 已确认的事实（21:05 - 22:15）

### 1. 设备时钟问题（关键根因之一）
- 21:05:10 设备看门狗复位后重启（Reset reason cpu0=11 = TGWDT_SYS），NTP 声称同步到 20:15:37 —— 这是过期的 RTC 值，实际真实北京时间已是 21:05（PC 时钟经 UTC 换算验证准确：UTC 13:13 = 北京 21:13）。
- 设备每 300 秒做一次 NTP 同步。t=300s（约 21:10:11）首次真正同步成功：时钟从 20:20:38 **跳变 +49.6 分钟**到 21:10:17。
- 这次时钟跳变直接**跳过了设备时钟的 20:30 半点 和 21:00 整点**两个报时点 → 调度器永远"看不见"这两个时刻 → 21:00 整点确认没有触发日志（复现了用户观察的"整点没报"模式）。
- 后续 NTP 同步正常：21:15:17 / 21:20:17 / 21:25:17，时钟保持准确。

### 2. 21:30 半点报时：触发成功，但演出挂死设备（关键根因之二）
- 21:30:00 设备日志:
  - `Half-hour chime trigger: 21:30`
  - `Half-hour chime: 21:30, starting mini performance`
  - `Performance: ENTER type=2 hour=21 min=30`
  - `AudioService: SET-MUTED: 1 (dq=0 pq=0 pushed=18 dropped=0)` ← 音频静音、播放队列清空
  - `AudioService: Output muted, playback queue cleared`
- **此后 111.txt 完全冻结（21:30:02 起），设备心跳（60s）、SystemInfo（10s）全部停止 → 设备挂死。** 演出未完成（无 Performance complete）。
- 与崩溃前日志一致的模式：`Last heartbeat before crash/reset: t=105750s state=3` —— state=3 疑似"演出中"状态，演出时挂起 → 看门狗复位。
- 截至 22:15 设备仍挂死（无复位）。

### 3. 用户原始观察的解释（推断）
- "20:00 整点没报"：当时设备时钟偏慢（RTC/NTP 问题）或设备正处于演出挂死状态 → 20:00 无报时。
- "20:30 报了"：设备时钟当时偏差较小或刚恢复 → 半点报时触发。
- 真正的机制：**报时触发器本身工作正常（21:30 证明），但 (a) 时钟偏慢/跳变导致报时点错位或跳过，(b) 每次演出开始后设备挂死 → 挂死期间所有报时丢失，直到看门狗复位。**

### 4. 待验证
- 设备何时恢复（看门狗复位 → 新启动日志）？
- 恢复后 22:00/22:30/23:00 整点/半点是否再次触发？演出是否再次挂死？（预期：每次演出都挂死）
- quiet_mode / is_dark / LDR 相关日志（目前未见，观察 22:00 后是否出现静音跳过逻辑）。

## 时间线（更新至 23:35）
- 21:05:10 重启完成（设备时钟 20:15:37，慢 ~50 分钟）
- 21:10:11 首次 NTP 真同步，时钟跳 +49.6 分钟 → 错过设备 20:30/21:00 报时点
- 21:30:00 半点报时触发 → 演出开始 → 21:30:02 设备挂死（音频静音+队列清空后）
- 21:30:02 ~ 22:19:48 设备挂死约 50 分钟（无任何日志，心跳/SystemInfo 全停）
- 22:19:48 看门狗复位（Reset reason cpu0=11 再次出现）→ 自动重启。**本次启动 NTP 直接同步正确时间 22:19:48（无时钟偏差，无跳变）**；LDR 阈值变为 500（首次启动为 600）
- 22:30:00 半点报时**正常触发**：CheckTime 22:30 dark=0 LDR=2023 thresh=500 quiet_mode=2 running=0 → Chime allowed（亮光）→ Performance ENTER type=2 → **8.4 秒后 Performance complete（无挂死！）**
- 23:00:00 整点报时**正常触发**：LDR=1654（亮）→ Chime allowed → Performance ENTER type=1（完整演出）→ **110.6 秒后 Performance complete（Door open done，堆内存从 3.4MB→2.99MB）**
- 23:30:00 半点报时**被跳过（设计行为）**：`CheckTime: 23:30 dark=1 LDR=0 thresh=500 quiet_mode=2` → `Skipping chime: quiet_mode=2 (dark-silent) LDR=0 threshold=500`。灯光已关（23:00 时 LDR=1654 → 23:30 时 LDR=0）
- 夜间预期：所有整点/半点因暗光静音被跳过（除非开灯）

## 报时判定逻辑（完整）
- `CheckTime: HH:MM dark=0/1 LDR=xxxx thresh=500 quiet_mode=2 running=0`
- LDR>=500（亮）：`Chime allowed: quiet_mode=2 but LDR=xxxx >= threshold=500 (bright)` → 触发演出
- LDR<500（暗）：`Skipping chime: quiet_mode=2 (dark-silent) LDR=xxx threshold=500` → 跳过（不演、不响）
- quiet_mode=2 = 暗光静音模式（按 LDR 自动）；深夜跳过报时是设计功能，非 bug
- 21:30 挂死 vs 22:30/23:00 成功：同一演出代码，前者挂死（疑似时钟跳变后音频/音乐代理状态异常或瞬时网络问题），后者均正常。挂死会导致看门狗 ~50 分钟后复位，期间报时全部丢失。

## 最终结果（监控结束 2026-08-03 08:00）
- 08:00 灯光打开（LDR=1926）→ `Chime allowed` → `Hourly chime: 8:00, starting performance` → Performance ENTER type=1（监控结束，未观测完成，但预期正常，参照 23:00 成功先例）
- 脚本最终统计：Hourly trigger=10、Half-hour trigger=11、Skipping chime=17（全部 dark-silent）、Performance ENTER=4、Performance complete=2（22:30、23:00）、Reset reason=2（两次看门狗复位）、E(错误行=6
- 整点：23:00 触发演出✔完成✔ | 00:00/01:00/02:00/03:00/04:00/05:00/06:00/07:00 暗光跳过 | 08:00 开灯后触发✔（21:00/22:00 因时钟跳变被调度器错过）
- 半点：21:30 触发→演出挂死 | 22:30 触发✔完成✔ | 23:30/00:30/01:30/02:30/03:30/04:30/05:30/06:30/07:30 暗光跳过（20:30 设备时刻被时钟跳变错过）
- 设备最终状态：健康运行中，NTP 每 300s 同步准确（07:59:52 最后一次）

## 根因结论
1. **整点/半点报时逻辑本身正常**（时钟准确时全部准时触发：21:30/22:30/23:00/08:00）
2. **时钟慢 ~50 分钟 + NTP 跳变**：启动时 RTC 残留旧时间，首次 NTP 同步跳变 +49.6min，跳变区间内的报时点（20:30/21:00 设备时刻）被调度器永久错过 → 解释了"整点没报"
3. **演出挂死 bug**：21:30 演出（音频静音+队列清空后）挂死设备 50 分钟，看门狗复位后才恢复；挂死期间一切报时丢失。22:30/23:00 演出正常，判断为偶发（疑似音频/音乐代理状态异常）
4. **夜间暗光静音是设计行为**：quiet_mode=2 下 LDR<500 一律跳过（Skipping chime: dark-silent），关灯后不报时是正常功能
5. 整点 vs 半点无本质差异：同为 CheckTime 判定+演出；差异仅来自时钟跳变/挂死/暗光三个外部因素
