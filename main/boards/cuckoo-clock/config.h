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
// I2C 总线（仅音频 codec，OLED 已拆除）
//   I2C_NUM_0 (GPIO1/2): ES8311(0x18) + ES7210(0x41)
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

// OLED 已拆除\uff0cGPIO42 改作 SERVO_DOG

// ============================================
// 布谷鸟钟专用引脚（新分配）
// ============================================

// --- 电机驱动板（统一 DRV8833, 2线 PWM 模式）---

// 板A: DRV8833 #1 — M1舞蹈 + M2大门 (2026-06-30: 大门从板C换到板A Timer0)
//   GPIO4,5,6,7
#define MOTOR_DANCE_IN1  GPIO_NUM_4     // M1 舞蹈
#define MOTOR_DANCE_IN2  GPIO_NUM_5
#define MOTOR_BIRD_IN1   GPIO_NUM_6     // M2 大门 (原鸟门，板子已互换)
#define MOTOR_BIRD_IN2   GPIO_NUM_7

// 板B: DRV8833 #2 — M1小狗 + M2小提琴
//   GPIO10,11,18,3
#define MOTOR_DOG_IN1    GPIO_NUM_10    // M1 小狗
#define MOTOR_DOG_IN2    GPIO_NUM_11
#define MOTOR_VIOLIN_IN1 GPIO_NUM_18    // M2 小提琴
#define MOTOR_VIOLIN_IN2 GPIO_NUM_45    // 从 GPIO3 移来\uff0c释放 GPIO3 给 LDR

// 板C: DRV8833 #3 — M1鸟门 + M2水车/鸟跳 (2026-06-30: 鸟门从板A换到板C Timer2)
//   GPIO9,46 (M1鸟门), GPIO8,39 (M2水车+鸟跳)
#define MOTOR_DOOR_IN1   GPIO_NUM_9     // M1 鸟门 (原大门，板子已互换)
#define MOTOR_DOOR_IN2   GPIO_NUM_46

// 板C M2: 水车 + 小鸟跳跃 (单向控制，各占一个IN脚)
#define MOTOR_WATER_BIRD_IN1  GPIO_NUM_8     // M2 IN1 → 水车电机
#define MOTOR_WATER_BIRD_IN2  GPIO_NUM_39    // M2 IN2 → 小鸟跳跃电磁铁

// --- 舵机信号线 (50Hz PWM) ---
// HIGH_SPEED mode (独立于电机低速通道，无冲突)
#define SERVO_VIOLIN      GPIO_NUM_40    // 小提琴手
#define SERVO_DOG         GPIO_NUM_42    // 小狗摇头

// --- RTC PCF8563 (I2C1, 地址 0x51) ---

// --- 光敏传感器（ADC 模拟读取）---
// 分压: VCC(3.3V) → LDR → GPIO3 → R_fix(200k) → GND
// ADC 实测范围: 全黑 ~100, 强光 ~3300 (12-bit ADC)
#define LDR_GPIO          GPIO_NUM_3     // ADC1_CH2
#define LDR_ADC_UNIT      ADC_UNIT_1
#define LDR_ADC_CHANNEL   ADC_CHANNEL_2
#define LDR_ADC_ATTEN     ADC_ATTEN_DB_12  // 0~3.3V 量程
#define LDR_ADC_BITWIDTH  ADC_BITWIDTH_12  // 0~4095

// 亮度阈值（12-bit ADC 原始值）
#define LDR_DARK          600             // <600 = 太暗，不报时

// --- LED 灯串 (S8050 驱动, GPIO→1KΩ→B极, C极→灯串负极, 灯串正极→5V) ---
#define LED_A_GPIO        GPIO_NUM_21
#define LED_B_GPIO        GPIO_NUM_41

// --- LEDC PWM 通道分配 ---
// Timer 0 (1kHz): 板A 电机 + 板B 电机 (8通道满)
//   板A M1舞蹈 + 板A M2大门 + 板B M1小狗 + 板B M2小提琴
#define LEDC_CH_DANCE_IN1   LEDC_CHANNEL_0   // M1 舞蹈 IN1
#define LEDC_CH_DANCE_IN2   LEDC_CHANNEL_1   // M1 舞蹈 IN2
#define LEDC_CH_BIRD_IN1    LEDC_CHANNEL_2   // M2 大门 IN1 (原鸟门，现大门)
#define LEDC_CH_BIRD_IN2    LEDC_CHANNEL_3   // M2 大门 IN2
#define LEDC_CH_DOG_IN1     LEDC_CHANNEL_4   // M1 小狗 IN1
#define LEDC_CH_DOG_IN2     LEDC_CHANNEL_5   // M1 小狗 IN2
#define LEDC_CH_VIOLIN_IN1  LEDC_CHANNEL_6   // M2 小提琴 IN1
#define LEDC_CH_VIOLIN_IN2  LEDC_CHANNEL_7   // M2 小提琴 IN2

// Timer 1 (50Hz): 舵机 — LOW_SPEED 独立通道 (ESP32-S3 无 HIGH_SPEED)
#define LEDC_CH_SERVO_V     LEDC_CHANNEL_0   // 小提琴手舵机 (timer1, LOW_SPEED)
#define LEDC_CH_SERVO_D     LEDC_CHANNEL_1   // 小狗舵机 (timer1, LOW_SPEED)
#define LEDC_TIMER_SERVO    LEDC_TIMER_1

// Timer 2 (10kHz): 板C 鸟门
#define LEDC_CH_DOOR_IN1    LEDC_CHANNEL_2   // M1 鸟门 IN1
#define LEDC_CH_DOOR_IN2    LEDC_CHANNEL_3   // door IN2
#define LEDC_TIMER_DOOR     LEDC_TIMER_2

// Timer 3 (10kHz): 板C 水车/鸟跳 — 从 Timer2 迁出，隔离干扰
#define LEDC_CH_WATER       LEDC_CHANNEL_0   // M2 水车 IN1 (timer3)
#define LEDC_CH_JUMP        LEDC_CHANNEL_1   // M2 鸟跳 IN2 (timer3)
#define LEDC_TIMER_WATER    LEDC_TIMER_3

// Timer 0 (1kHz): 板A+板B 电机共用
#define LEDC_TIMER_MOTOR    LEDC_TIMER_0

// ============================================
// 电机参数（速度和时长）
// ============================================

// 大门电机 (m2_, 板A M2)
#define MAIN_DOOR_OPEN_SPEED   50      // 大门开启速度(% duty)
#define MAIN_DOOR_CLOSE_SPEED  50      // 大门关闭速度(% duty)
#define MAIN_DOOR_TIME_MS      1400    // 大门动作时长(ms)

// 鸟门电机 (m4_, 板C M1)
#define BIRD_DOOR_OPEN_SPEED   70      // 鸟门开启速度(% duty)
#define BIRD_DOOR_CLOSE_SPEED  70      // 鸟门关闭速度(% duty)
#define BIRD_DOOR_TIME_MS      600     // 鸟门动作时长(ms)

// 舞蹈电机参数 (板A M1)
#define DANCE_SPEED_PERCENT    80      // 舞蹈电机速度(% duty)

// 小提琴电机参数 (板B M2)
#define VIOLIN_SPEED_PERCENT   100     // 小提琴电机速度(% duty)
#define VIOLIN_WARMUP_PERCENT  60      // 小提琴预热速度(% duty)
#define VIOLIN_BALANCE_TIME_MS 1200   // 正反转平衡补偿时长

// 小狗电机参数 (板B M1)
#define DOG_SPEED_PERCENT      20      // 小狗电机速度(% duty)
#define DOG_WALK_TIME_MS       1000     // 前进/后退持续时间

// 小狗舵机参数
#define DOG_TAIL_SWEEP_START   180     // 摇尾巴起始角度
#define DOG_TAIL_SWEEP_END     20      // 摇尾巴结束角度
#define DOG_TAIL_STEP_MS       15      // 每步间隔(ms)

// 水车参数 (板C M2)
#define WATER_WHEEL_SPEED      100     // 水车转速(% duty)

// 舵机默认角度
#define SERVO_CENTER_ANGLE     90      // 舵机归中角度

// ============================================
// 在线音乐代理
// ============================================
#define DEFAULT_MUSIC_PROXY_HOST    "120.55.47.160"
#define DEFAULT_MUSIC_PROXY_PORT    8765

// ============================================
// 角色表演音乐编号（TF卡 assets 目录）
// ============================================
#define LINDA_SONG_BASE     3001        // Linda Show 歌曲起始编号
#define LINDA_SONG_COUNT    1           // Linda Show 歌曲数量
#define GARDEN_SONG_BASE    4001        // Garden Show 歌曲起始编号
#define GARDEN_SONG_COUNT   1           // Garden Show 歌曲数量

#endif // _BOARD_CONFIG_H_
