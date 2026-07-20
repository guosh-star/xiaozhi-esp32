#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================
// ES7210+ES8311+NS4150B AEC CODEC 音频模块
//   已验证可用的卖家引脚配置
//   SDA→GPIO1(J3), SCL→GPIO2(J3)
//   MCLK→GPIO15(J1), WS→GPIO13(J1), BCK→GPIO14(J1)
//   DIN→GPIO12(J1), DOUT→GPIO16(J1), EN→GPIO17(J1)
// ============================================
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// 回声消除参考通道 (ES7210: CH0=语音, CH1=参考)
#define AUDIO_INPUT_REFERENCE    true

// I2S 音频接口（卖家验证配置）
#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_15   // 主时钟
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_14   // 位时钟
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_13   // 字选
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_16   // ESP32→ES8311
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_12   // ES7210→ESP32

// ES8311 + ES7210 I2C 地址
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  // 0x18
#define AUDIO_CODEC_ES7210_ADDR  0x82                       // 8-bit, 7-bit=0x41

// NS4150B 功放控制
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_17     // NS4150B EN (HIGH=开)
#define AUDIO_CODEC_EN_PIN       GPIO_NUM_NC     // VCC 常通 5V

// ============================================
// I2C 总线分离（避免音频 codec 与 OLED 冲突导致 I2C 死锁）
//   I2C_NUM_0 (GPIO1/2): ES8311(0x18) + ES7210(0x41) — 音频 codec 独占
//   I2C_NUM_1 (GPIO41/42): SSD1306(0x3C) — 显示独占
//   PCF8563(0x51) 未使用
// ============================================
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2

// 按键
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47

// LED（DevKitC-1 板载 WS2812，不用于本项目，拉低省电）
#define BUILTIN_LED_GPIO        GPIO_NUM_48  // WS2812 状态指示灯

// 电机驱动电源控制 (P-MOSFET, LOW=供电, HIGH=断电)
#define POWER_MOTOR_GPIO        GPIO_NUM_38

// OLED 屏幕 (I2C_NUM_1: SDA=GPIO41, SCL=GPIO42，不再与音频共享总线)
#define DISPLAY_SDA_PIN         GPIO_NUM_41
#define DISPLAY_SCL_PIN         GPIO_NUM_42
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64
#define DISPLAY_MIRROR_X        true
#define DISPLAY_MIRROR_Y        true

// ============================================
// 布谷鸟钟专用引脚（新分配）
//   J1 整排 → 电机驱动
//   J3 整排 → 外设集中
// ============================================

// --- DRV8833 驱动板 (M1 舞蹈 + M2 小提琴) ---
// 2线 PWM 模式，IN1/IN2 各一个 LEDC 通道
#define DRV8833_M1_IN1      GPIO_NUM_4
#define DRV8833_M1_IN2      GPIO_NUM_5
#define DRV8833_M2_IN3      GPIO_NUM_6
#define DRV8833_M2_IN4      GPIO_NUM_7

// --- TB6612 驱动板 (M3 水车 + M4 大门) ---
// 3线控制，每路电机 1 个 PWM + 2 个方向脚
#define TB6612_2_PWMA      GPIO_NUM_8     // M3 PWM
#define TB6612_2_AIN1      GPIO_NUM_3     // M3 方向1
#define TB6612_2_AIN2      GPIO_NUM_18    // M3 方向2
#define TB6612_2_PWMB      GPIO_NUM_9     // M4 PWM
#define TB6612_2_BIN1      GPIO_NUM_10    // M4 方向1
#define TB6612_2_BIN2      GPIO_NUM_11    // M4 方向2

// --- 鸟门电机 (独立 H桥, 2脚方向控制) ---
#define BIRD_DOOR_DIR1     GPIO_NUM_21    // J3
#define BIRD_DOOR_DIR2     GPIO_NUM_46    // J1-46（唯一在 J1 的非电机脚）

// --- 小鸟跳跃电磁铁 ---
#define BIRD_JUMP_GPIO     GPIO_NUM_39

// --- 舵机信号线 (50Hz PWM) ---
#define SERVO_VIOLIN       GPIO_NUM_40
#define SERVO_DOG          GPIO_NUM_33    // J3-17，空闲可用

// --- RTC PCF8563 (I2C1, 地址 0x51) ---
// 和 OLED 共享 I2C1 总线 (SDA=42, SCL=41)

// --- 光敏传感器（ADC / 数字输入）---
#define LDR_GPIO           GPIO_NUM_45    // strapping MTDI, 启动后可用

// --- LEDC PWM 通道分配 ---
// DRV8833: 每路电机占 2 个 LEDC 通道（IN1+IN2 独立 PWM）
#define LEDC_CH_M1_AIN1    LEDC_CHANNEL_0   // M1 舞蹈 IN1
#define LEDC_CH_M1_AIN2    LEDC_CHANNEL_1   // M1 舞蹈 IN2
#define LEDC_CH_M2_AIN1    LEDC_CHANNEL_2   // M2 小提琴 IN1
#define LEDC_CH_M2_AIN2    LEDC_CHANNEL_3   // M2 小提琴 IN2
#define LEDC_CH_SERVO_V    LEDC_CHANNEL_4   // 小提琴手舵机
#define LEDC_CH_SERVO_D    LEDC_CHANNEL_5   // 小狗舵机
// CH6-7 预留（TB6612 M3/M4 电机后续接�?

// ============================================
// 在线音乐代理
// ============================================
#define DEFAULT_MUSIC_PROXY_HOST    "120.55.47.160"
#define DEFAULT_MUSIC_PROXY_PORT    8765

#endif // _BOARD_CONFIG_H_
