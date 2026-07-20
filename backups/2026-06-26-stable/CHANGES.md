# 2026-06-26 稳定版备份

## 改动汇总（7 文件）

### 1. PlayOpus 保护 (cuckoo_controller.cc)
- `PlayOpus()`: 播放中拒绝换歌，直接 return -1
- AI 要换歌必须先调 cuckoo.stop_music

### 2. I2C 总线分离 (cuckoo_board.cc)
- 音频 codec → I2C_NUM_0 (GPIO1=SDA, GPIO2=SCL)
- SSD1306 OLED → I2C_NUM_1 (GPIO42=SDA, GPIO41=SCL)
- 彻底解决 I2C 死锁导致 task_wdt 死机

### 3. WS2812 状态灯 (ws2812_led.h/cc 新建)
- GPIO48 驱动板载 WS2812
- 状态映射：灭=休眠, 暗白=启动, 黄闪=配网, 紫=激活, 蓝=连接, 绿=聆听, 红=说话, 紫慢闪=升级, 橙=致命错
- FreeRTOS 定时器实现闪烁

### 4. AEC 升级 (afe_audio_processor.cc)
- AFE_MODE_LOW_COST → AFE_MODE_HIGH_PERF
- AEC_MODE_VOIP_LOW_COST → AEC_MODE_VOIP_HIGH_PERF
- 改善回声消除质量，减少误消语音

### 5. 编译配置 (CMakeLists.txt)
- 添加 led/ws2812_led.cc

### 6. 引脚配置更新 (config.h)
- BUILTIN_LED_GPIO → GPIO_NUM_48
- DISPLAY_SDA/SCL_PIN → GPIO_NUM_42/41
- I2C 注释更新为总线分离方案

## 验证结果
- ✅ I2C 零错误，无死机
- ✅ 音乐 ducking 正常
- ✅ 唤醒词 + 音乐共存
- ✅ Heartbeat 栈水位 6692，空闲堆 ~5MB
- ✅ minimal SRAM 17999（安全）
- ✅ WS2812 状态灯正常
