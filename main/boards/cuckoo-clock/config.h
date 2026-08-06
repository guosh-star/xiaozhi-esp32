#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ============================================
// 音频采样率配置
//
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// 启用参考信号通道（AEC 回声消除用）
#define AUDIO_INPUT_REFERENCE    true

// I2S 音频接口引脚
#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_15 // 主时钟
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_14 // 位时钟
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_13 // 字选/左右声道
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_16 // 数据输出（DAC→扬声器）
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_12 // 数据输入（麦克风→ADC）

// 音频 Codec 芯片地址
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR  // DAC 0x18
#define AUDIO_CODEC_ES7210_ADDR  0x82                       // ADC 8-bit地址，7-bit=0x41

// Codec 控制引脚
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_17 // 功放使能
#define AUDIO_CODEC_EN_PIN       GPIO_NUM_NC // 复位（未使用）

// ============================================
// I2C 总线配置
//   I2C_NUM_0 (GPIO1/2): 连接 ES8311(0x18) + ES7210(0x41)
//
// ============================================
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_1
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_2

// 系统按键引脚
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define TOUCH_BUTTON_GPIO       GPIO_NUM_47

// 内置 LED 指示灯
#define BUILTIN_LED_GPIO        GPIO_NUM_48 // IO48

// P-MOSFET 电机总电源控制（高=断电，低=通电）
#define POWER_MOTOR_GPIO        GPIO_NUM_38

// LEDC PWM 定时器分配
// ============================================
// 四路主电机（TB6612 驱动，定时器0）
// ============================================

// M1 舞蹈电机 — GPIO4,5
//   引脚组：GPIO4,5（M1 舞蹈）+ GPIO6,7（M2 大门）
#define MOTOR_DANCE_IN1  GPIO_NUM_4 // M1 正转
#define MOTOR_DANCE_IN2  GPIO_NUM_5
#define MOTOR_BIRD_IN1   GPIO_NUM_6 // M2 大门正转（board.cc m2_ 实际用途）
#define MOTOR_BIRD_IN2   GPIO_NUM_7 // M2 大门反转

// M4 小鸟门电机 — GPIO9,46 | M3 小狗电机 — GPIO10,11 | 小提琴升降电机 — GPIO18,45
//   引脚组：GPIO10,11（M3 小狗）+ GPIO18,45（小提琴升降）+ GPIO9,46（M4 鸟门）
#define MOTOR_DOG_IN1    GPIO_NUM_10 // M3 小狗正转
#define MOTOR_DOG_IN2    GPIO_NUM_11
#define MOTOR_VIOLIN_IN1 GPIO_NUM_18 // 小提琴升降电机正转
#define MOTOR_VIOLIN_IN2 GPIO_NUM_45 // 小提琴升降电机反转（注：实际脚号为45）

// 鸟门电机（PWM 驱动，独立定时器2）
//
#define MOTOR_DOOR_IN1   GPIO_NUM_9  // M4 小鸟门正转（board.cc m4_ 实际用途）
#define MOTOR_DOOR_IN2   GPIO_NUM_46 // M4 小鸟门反转

// 水车+鸟跳电机（DRV8833 独立控制，定时器3）
#define MOTOR_WATER_BIRD_IN1  GPIO_NUM_8  // 水车旋转
#define MOTOR_WATER_BIRD_IN2  GPIO_NUM_39 // 鸟跳驱动

// 舵机引脚（2克微型舵机，定时器1）
//
#define SERVO_VIOLIN      GPIO_NUM_40 // 小提琴手臂舵机
#define SERVO_DOG         GPIO_NUM_42 // 狗尾舵机

// 光敏电阻（ADC 检测昼夜）
//
#define LDR_GPIO          GPIO_NUM_3     // ADC1_CH2
#define LDR_ADC_UNIT      ADC_UNIT_1
#define LDR_ADC_CHANNEL   ADC_CHANNEL_2
#define LDR_ADC_ATTEN     ADC_ATTEN_DB_12 // 12dB 衰减
#define LDR_ADC_BITWIDTH  ADC_BITWIDTH_12  // 12-bit 分辨率，0~4095

// 暗光判定阈值
#define LDR_DARK          500 // ADC 值低于此判定为暗

// WS2812 LED 灯带引脚（A/B 两路）
#define LED_A_GPIO        GPIO_NUM_21
#define LED_B_GPIO        GPIO_NUM_41

// LEDC PWM 通道分配（定时器0，四路主电机）
//
#define LEDC_CH_DANCE_IN1   LEDC_CHANNEL_0 // M1 正转
#define LEDC_CH_DANCE_IN2   LEDC_CHANNEL_1 // M1 反转
#define LEDC_CH_BIRD_IN1    LEDC_CHANNEL_2 // M2 大门正转（PWM 备用，board.cc 中 m2_ 用 GPIO 直驱）
#define LEDC_CH_BIRD_IN2    LEDC_CHANNEL_3 // M2 大门反转（PWM 备用）
#define LEDC_CH_DOG_IN1     LEDC_CHANNEL_4 // M3 正转
#define LEDC_CH_DOG_IN2     LEDC_CHANNEL_5 // M3 反转
#define LEDC_CH_VIOLIN_IN1  LEDC_CHANNEL_6 // 小提琴升降正转
#define LEDC_CH_VIOLIN_IN2  LEDC_CHANNEL_7 // 小提琴升降反转

// 舵机 PWM 通道（定时器1）
#define LEDC_CH_SERVO_V     LEDC_CHANNEL_0 // 小提琴舵机
#define LEDC_CH_SERVO_D     LEDC_CHANNEL_1 // 狗尾舵机
#define LEDC_TIMER_SERVO    LEDC_TIMER_1

// 大门电机 PWM 通道（定时器2）
#define LEDC_CH_DOOR_IN1    LEDC_CHANNEL_2 // M4 小鸟门正转（PWM 驱动）
#define LEDC_CH_DOOR_IN2    LEDC_CHANNEL_3 // M4 小鸟门反转（PWM 驱动）
#define LEDC_TIMER_DOOR     LEDC_TIMER_2

// 水车+鸟跳 PWM 通道（定时器3）
#define LEDC_CH_WATER       LEDC_CHANNEL_0 // 水车
#define LEDC_CH_JUMP        LEDC_CHANNEL_1 // 鸟跳
#define LEDC_TIMER_WATER    LEDC_TIMER_3

// 主电机 PWM 定时器（四路共用）
#define LEDC_TIMER_MOTOR    LEDC_TIMER_0

// ============================================
// 电机运行参数
// ============================================

// 大门控制参数
#define MAIN_DOOR_OPEN_SPEED   50 // 开门速度（%占空比）
#define MAIN_DOOR_CLOSE_SPEED  50 // 关门速度
#define MAIN_DOOR_TIME_MS      1400 // 开门/关门持续时间 (ms)

// 小鸟门控制参数
#define BIRD_DOOR_OPEN_SPEED   70 // 鸟门打开速度
#define BIRD_DOOR_CLOSE_SPEED  70 // 鸟门关闭速度
#define BIRD_DOOR_TIME_MS      600 // 鸟门动作时间

// 舞蹈电机参数
#define DANCE_SPEED_PERCENT    80 // 舞蹈电机速度（%）

// 小提琴电机参数
#define VIOLIN_SPEED_PERCENT   100 // 正常运行速度
#define VIOLIN_WARMUP_PERCENT  60 // 启动预热速度
#define VIOLIN_BALANCE_TIME_MS 1200 // 平衡归位时间

// 小狗电机参数
#define DOG_SPEED_PERCENT      20 // 狗行走速度（%）
#define DOG_WALK_TIME_MS       1000 // 狗行走持续时间

// 狗尾舵机参数
#define DOG_TAIL_SWEEP_START   180 // 收起步角度
#define DOG_TAIL_SWEEP_END     20 // 出场终点角度
#define DOG_TAIL_STEP_MS       15 // 每步持续时间

// 水车速度
#define WATER_WHEEL_SPEED      100 // 水车转速（%）

// 舵机中立角度
#define SERVO_CENTER_ANGLE     90 // 舵机中立位 90°（2克微型舵机）

// ============================================
// QQ 音乐代理服务器配置
// ============================================
#define DEFAULT_MUSIC_PROXY_HOST    "120.55.47.160"
#define DEFAULT_MUSIC_PROXY_PORT    8765

// ============================================
// 演出音乐编号配置
// ============================================
#define LINDA_SONG_BASE     3001 // 琳达主题曲起始编号
#define LINDA_SONG_COUNT    1    // 琳达歌曲数量
#define GARDEN_SONG_BASE    4001 // 花园主题曲起始编号
#define GARDEN_SONG_COUNT   1    // 花园歌曲数量

#endif // _BOARD_CONFIG_H_
