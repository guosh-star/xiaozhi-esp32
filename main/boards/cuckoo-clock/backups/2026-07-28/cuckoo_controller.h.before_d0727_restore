#ifndef __CUCKOO_CONTROLLER_H__
#define __CUCKOO_CONTROLLER_H__

#include "mcp_server.h"
#include "config.h"
#include "assets.h"
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

// MP3 软件解码器
#include <esp_audio_types.h>
#include <decoder/esp_audio_dec.h>
#include <decoder/impl/esp_mp3_dec.h>

// 唤醒提示音
#include "cuckoo_wake_sound.h"

// OGG/Opus 解封装器（用于原生 Opus 解码管线）
#include "ogg_demuxer.h"

// ============================================
// 电机驱动封装类
// ============================================

/**
 * @brief 电机驱动封装（支持 GPIO 直驱 / PWM 双模式）
 *
 * 三个构造函数对应不同硬件场景：
 * - Motor(in1, in2): GPIO 直驱 → L9110S 等简单 H 桥芯片
 * - Motor(in1, in2, ch1, ch2, speed_mode): PWM 共享定时器 → TB6612 四路主电机（Timer 0, 1kHz）
 * - Motor(in1, in2, ch1, ch2, timer, speed_mode): PWM 自定义定时器 → DRV8833 独立调速（8-bit, 10kHz）
 */
class Motor {
private:
    gpio_num_t in1_pin_;
    gpio_num_t in2_pin_;
    bool use_pwm_;
    // PWM 模式相关字段
    ledc_channel_t ledc_channel_;
    ledc_channel_t ledc_channel2_;
    ledc_timer_t ledc_timer_;
    ledc_mode_t speed_mode_;
    uint32_t max_duty_;  // 最大占空比：8-bit=255，10-bit=1023

public:
    // GPIO 直驱模式构造函数
    Motor(gpio_num_t in1_pin, gpio_num_t in2_pin);
    // PWM 模式构造函数（共享定时器）
    Motor(gpio_num_t in1_pin, gpio_num_t in2_pin, ledc_channel_t ch1, ledc_channel_t ch2,
          ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
    // PWM 模式构造函数（自定义定时器）
    Motor(gpio_num_t in1_pin, gpio_num_t in2_pin, ledc_channel_t ch1, ledc_channel_t ch2,
          ledc_timer_t timer, ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
    void SetSpeed(int speed);
    void Stop();
    void Forward(int speed = 100);
    void Reverse(int speed = 100);
};

/**
 * SG90 舵机封装
 * 使用 PWM 控制，50Hz，0.5-2.5ms 脉冲
 */
class Servo {
private:
    gpio_num_t pin_;
    int angle_;                              // 当前角度 0°~180°
    ledc_channel_t ledc_channel_;
    ledc_mode_t speed_mode_;                 // LOW_SPEED or HIGH_SPEED

public:
    Servo(gpio_num_t pin, ledc_channel_t channel,
          ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
    void SetAngle(int angle);                // 设置 0°~180°
    int GetAngle() { return angle_; }
    void Sweep(int from, int to, int duration_ms = 500); // 平滑扫过角度
};

/**
 * @brief 水车+鸟跳：两个独立单向设备，通过 DRV8833 双通道独立控制
 *
 * 水车电机（一脚接 GPIO8，一脚接地）→ 单向旋转
 * 鸟跳电磁铁（一脚接 GPIO39，一脚接地）→ 单向脉冲
 * 使用 Motor 构造函数 #3（自定义定时器 Timer 3，8-bit，10kHz）：
 * - SetSpeed(WATER_WHEEL_SPEED) → 水车旋转
 * - SetSpeed(-100) → 鸟跳一次（短脉冲）
 */

/**
 * 钟声合成播放器
 */
class BellSoundPlayer {
public:
    BellSoundPlayer() = default;
    ~BellSoundPlayer() = default;

    // 初始化
    bool Init() { return true; }

    // 播放布谷鸟叫声
    void PlayCuckooSoundSync();
    void PlayBellSoundSync();

    // 异步播放布谷鸟叫声
    void PlayCuckooSoundAsync();

    // 是否已初始化
    bool IsInitialized() { return true; }
};

// RTC (已废弃，时间通过 NTP 同步)

/**
 * @brief MP3 软件解码播放器
 *
 * 从 assets 分区读取 MP3 文件，用 esp_mp3_dec 软解码，
 * 通过 AudioService 输出到 I2S。支持 HTTP 流式下载（PlayUrl/PlayOpus/PlayPcm）。
 * 各播放方法内部 spawn 独立 FreeRTOS task，MCP 调用不阻塞。
 */
class Mp3Player {
private:
    Assets* assets_ = nullptr;
    void* mp3_dec_handle_ = nullptr;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<float> ducking_gain_{1.0f};     // 1.0=正常, 0.0=静音(平滑淡入淡出)
    std::atomic<int64_t> ducking_start_us_{0};  // 淡入/淡出开始时间
    std::atomic<bool> disable_ducking_{false};  // 禁用 Ducking

    // 播放任务句柄
    TaskHandle_t play_task_ = nullptr;
    int pending_track_ = 0;              // 待播放曲目
    int pending_bell_hour_ = 0;          // 待播放报时小时数

    // 音频缓冲区
    static constexpr size_t kInputBufSize = 8192;  // 输入缓冲
    static constexpr size_t kOutputBufSize = 8192; // 输出缓冲
    uint8_t input_buf_[kInputBufSize];
    uint8_t output_buf_[kOutputBufSize];

    // 钟声播放器
    BellSoundPlayer bell_player_;

    // 狗叫 PCM 缓存
    std::vector<int16_t> bark_pcm_;
    size_t bark_total_ = 0;
    size_t bark_offset_ = 0;
    std::atomic<bool> bark_active_{false};

    // 播放任务入口
    static void PlayTaskEntry(void* arg);

    // Ducking 状态管理和增益处理
    void UpdateDuckingState();
    void ApplyDuckingGain(int16_t* pcm, size_t num_samples);

public:
    Mp3Player() = default;
    ~Mp3Player();

    void Init(Assets* assets);

    // 解码并播放单个 MP3 文件
    int DecodeSingleFile(int index);

    // 解码 MP3 到内存缓冲区（不直接播放）
    int DecodeToBuffer(int index, int16_t** out_buf, size_t* out_samples, int* out_samplerate);

    void PlayTrack(uint8_t folder, uint8_t track); // 播放指定曲目
    void PlayIndex(uint16_t index);                // 按全局索引播放
    void Stop();
    void ResetForNextPlay();
    void Pause();
    void Resume();
    void SetVolume(uint8_t vol);
    void VolumeUp();
    void VolumeDown();
    void Next();
    void Prev();
    void PlayBell(int hour);                       // 播放报时钟声
    void PlayBgMusic(int index);                   // 播放背景音乐
    void PlayAlarmRing(float volume = 1.0f);       // 播放闹钟铃声
    int PlayUrl(const char* url);                  // HTTP 播放 MP3
    int PlayOpus(const char* url);                 // HTTP 播放 Opus
    int PlayPcm(const char* url);                  // HTTP 播放原始 PCM
    static void PlayUrlTask(void* arg);            // HTTP MP3 异步任务
    static void PlayOpusTask(void* arg);           // HTTP Opus 异步任务
    static void PlayPcmTask(void* arg);            // HTTP PCM 异步任务
    bool IsPlaying() { return is_playing_; }
    void SetDisableDucking(bool disable) { disable_ducking_ = disable; }

    // 加载狗叫 PCM 数据到缓存
    void LoadDogBark(const int16_t* pcm, size_t num_samples);
    bool IsBarkActive() { return bark_active_.load(); }
};

/**
 * 光敏电阻传感器 (ADC 模拟读取，昼夜检测)
 */
class LdrSensor {
private:
    gpio_num_t adc_pin_;
    adc_oneshot_unit_handle_t adc_handle_;
    adc_channel_t adc_chan_;
    int threshold_;                              // 暗光判定阈值

public:
    LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold = 2000);
    ~LdrSensor();
    int ReadRaw();
    bool IsDark();
    void SetThreshold(int threshold);
};

/**
 * @brief 布谷鸟钟状态机 — 管理所有硬件动作和表演序列
 *
 * 负责：报时（整点/半点）、闹钟、三场角色秀（狗/琳达/花园）、
 * 综合表演（start_show）、在线音乐播放、粤语查询等。
 * 所有耗时操作通过 spawn 独立 FreeRTOS task 避免阻塞。
 */
class CuckooStateMachine {
public:
    // 演出类型枚举
    enum PerformanceType {
        kPerformanceNone,                          // 无演出
        kPerformanceHour,                          // 整点报时
        kPerformanceHalf,                          // 半点报时
        kPerformanceManual,                        // 手动触发表演
        kPerformanceBgMusic,                       // 背景音乐模式
    };

    // 报时阶段枚举
    enum Phase {
        kPhaseIdle,
        kPhaseOpeningDoor,                         // 打开大门
        kPhaseBirdOut,                             // 小鸟探出
        kPhaseCalling,                             // 布谷鸟叫
        kPhaseBirdIn,                              // 小鸟缩回
        kPhaseClosingDoor,                         // 关闭大门
        kPhaseChiming,                             // 钟声演奏
        kPhaseDance,                               // 舞蹈阶段
        kPhaseDone,                                // 完成
    };

    // 整点/半点去重检测（防止同一小时内重复触发）
    bool NeedHourlyChime(int hour) const { return hour != last_hour_; }
    bool NeedHalfHourlyChime(int hour) const { return hour != last_half_hour_; }
    void MarkHourlyChime(int hour) { last_hour_ = hour; }
    void MarkHalfHourlyChime(int hour) { last_half_hour_ = hour; }

private:
    // 外设指针
    Motor* m1_;              // M1 舞蹈电机
    Motor* m2_;              // M2 大门电机
    Motor* m3_;              // M3 小狗电机
    Motor* m4_;              // M4 小鸟门电机
    Motor* violin_motor_;    // 小提琴升降电机
    Servo* violin_servo_;    // 小提琴手臂舵机
    Servo* dog_servo_;       // 狗尾舵机
    Motor* water_bird_;      // 水车+鸟跳电机
    Mp3Player* mp3_;
    BellSoundPlayer* bell_player_;
    // RtcPcf8563* rtc_;  // [已废弃] 使用 NTP 同步
    LdrSensor* ldr_;
    gpio_num_t motor_power_pin_ = GPIO_NUM_NC;

    PerformanceType current_performance_;
    Phase current_phase_;
    int call_count_;                           // 布谷鸟叫声计数
    int total_calls_;                          // 布谷鸟叫声总次数
    std::atomic<bool> is_running_{false};
    int last_hour_;                            // 上次整点报时的小时
    int last_half_hour_;                       // 上次半点报时的小时
    int show_music_index_;                   // 当前播放曲目编号
    int music_dance_phase_ = 0;              // 音乐舞蹈相位 (0-7)
    int music_dance_enabled_ = -1;           // 音乐舞蹈状态: -1=未初始化, 0=已停止, 1=激活中
    int m1_music_fwd_count_ = 0;               // 音乐中 M1 正转脉冲计数
    int m1_music_rev_count_ = 0;               // 音乐中 M1 反转脉冲计数
    bool dog_outro_done_ = false;
    std::atomic<bool> dog_outro_running_{false};  // 小狗回家动画进行中
    std::atomic<bool> dog_intro_running_{false};  // 小狗出场动画进行中（防重复创建）
    std::atomic<bool> dog_intro_done_{false};     // 小狗出场完成
    std::atomic<bool> kids_active_{true};         // Kids 模式是否激活（默认true，启动即活跃）

    // 小提琴循环状态
    struct ViolinLoopState {
        int stage = 0;
        int loop_count = 0;
        int timer = 0;
        int angle = 90;
        int fwd_count = 0;
        int rev_count = 0;
        void Reset() { stage = 0; loop_count = 0; timer = 0; angle = 90; fwd_count = 0; rev_count = 0; }
    };
    ViolinLoopState violin_state_;

    // 狗尾摇摆状态
    struct DogTailState {
        int angle = 0;
        int dir = 1;
        int target = 50;
        int pause = 0;
        void Reset() { angle = 0; dir = 1; target = 50; pause = 0; }
    };
    DogTailState dog_state_;
    std::atomic<bool> door_open_{false};   // 大门状态
    std::atomic<bool> dog_out_{false};    // 小狗状态
    std::atomic<bool> kids_dance_{false}; // KidsDanceShow 触发标志

    // M1 舞蹈电机时序
    unsigned long m1_fwd_time_ = 0;
    unsigned long m1_rev_time_ = 0;
    unsigned long m1_stage_start_ = 0;

public:
    // 时间状态
    std::atomic<int> current_hour_{0};         // 当前小时
    std::atomic<int> current_min_{0};          // 当前分钟
    std::atomic<int> current_sec_{0};          // 当前秒
    std::atomic<bool> time_set_{false};        // 时间是否已通过 NTP 设定
    std::atomic<bool> is_dark_{false};         // 当前是否暗光

    // 唤醒时间戳
    uint64_t last_idle_exit_us_ = 0;
    int prev_device_state_ = -1;               // 上一次设备状态

    // 闹钟管理（最多 5 个）
    static constexpr int kMaxAlarms = 5;

    struct AlarmInfo {
        int hour;
        int minute;
        bool enabled;
        bool repeat_daily;                     // 是否每日重复
    };

    mutable std::mutex alarm_mutex_;
    AlarmInfo alarms_[kMaxAlarms] = {};
    std::atomic<int> alarm_count_{0};
    std::atomic<bool> alarm_ringing_{false};
    std::atomic<bool> alarm_stopped_{false};
    std::atomic<int> thanks_counter_{0};       // 感谢语计数
    std::atomic<int> linda_song_counter_{0};   // 琳达歌曲计数
    std::atomic<int> garden_song_counter_{0};  // 花园歌曲计数

    // 静音模式配置
    std::atomic<int> quiet_mode_{2};           // 0=全天静音 1=全天报时 2=光线静音(LDR) 3=时间段静音
    std::atomic<int> quiet_start_{22};         // 静音开始小时
    std::atomic<int> quiet_end_{6};            // 静音结束小时
    std::atomic<bool> hourly_perf_{true};      // 整点报时后是否附加演出（音乐+舞蹈）

    void SaveQuietMode();
    void LoadQuietMode();
    void SaveKidsActive();
    void LoadKidsActive();
    void SaveHourlyPerf();
    void LoadHourlyPerf();

    void SetAlarm(int hour, int minute, bool repeat_daily);
    std::string GetAlarmsJson();
    bool DeleteAlarm(int index);  // 1-based 索引
    bool IsAlarmRinging() const { return alarm_ringing_; }
    
    // 闹钟持久化（NVS 存储，断电保留）
    void SaveAlarmsToNvs();
    void LoadAlarmsFromNvs();
    void StopAlarm();
    void CheckAlarms(int hour, int minute, int sec);
    static void AlarmTask(void* arg);

    CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                       Motor* violin_motor, Servo* violin, Servo* dog,
                       Motor* water_bird, Mp3Player* mp3,
                       BellSoundPlayer* bell_player,
                       // RtcPcf8563* rtc,  // [已废弃] 使用 NTP
                       LdrSensor* ldr);
    ~CuckooStateMachine();

    // 电机电源控制（P-MOSFET：低=通电，高=断电）
    void MotorPowerOn();
    void MotorPowerOff();

    // 启动报时/演出异步流程
    void StartPerformance(PerformanceType type, int hour = 0);

    // 音乐状态查询
    bool IsMusicPlaying() { return mp3_ && mp3_->IsPlaying(); }
    void StopMusic();

    // 报时/演出异步任务入口（运行在 Core 1）
    static void PerformanceTask(void* arg);

    // 舞蹈循环（音乐播放时的节奏动作）
    void RunDanceLoop();
    void RunDanceFinale();
    void RunDanceIntro();
    static void DoorOpenTask(void* arg);
    void PlayDogBark();                        // 播放狗叫声

    // 时间检测和报时触发
    void CheckTime(int hour, int min, bool dark);

    // 基础控制
    void Dance();
    void MotorTest(int seconds);
    void StartShow();
    void StartShowTask();
    static void ShowTask(void* arg);           // 综合演出异步任务
    void PlayMusic(int index);
    void StopAll();
    void OpenDoor();
    void CloseDoor();
    void BirdJumpOnce();
    void OpenBirdDoor();
    void CloseBirdDoor();
    void PlayCuckooSound();
    void BirdJumpPulse();
    void BirdJumpShort();
    void MusicDanceTick();                     // 音乐播放时舞蹈电机 + 舵机节奏摆动
    void MusicDogIntro();                      // 音乐开始时小狗出场（不叫）
    void MusicDogOutro();                      // 音乐结束时小狗回家
    void KidsComeOut();
    void KidsRest();
    void KidsDanceShow();
    static void AutoCloseTimerCallback(TimerHandle_t timer);
    void SetServoAngle(int servo_id, int angle);
    void SetMotorSpeed(int motor_id, int speed);

    // 特殊表演
    void DogShow();
    void StartLindaShow();
    void StartGardenShow();
    void LindaShow();                          // 琳达主题演出
    void GardenShow();                         // 花园主题演出
    void DogShowTask();                        // 小狗演出异步任务
    void PlayDogBarkDirect();
    void PlayWavAsset(const char* filename);
    bool PlayShowMusicBg(int index);           // 播放演出背景音乐（环形缓冲）
    void SetBirdDoorSpeed(int speed);

    // 在线音乐
    int PlayOnlineMusic(const char* url);
    std::string CheckMultiArtist(const char* url_or_path);

    // 粤语查询（通过代理的 /cantonese 端点同步查询）
    std::string CantoneseLookup(const char* word);

    struct MusicTaskCtx {
        CuckooStateMachine* sm;
        char url[512];
    };
    static void PlayOnlineMusicTask(void* arg);

    // 时间设置
    void SetTime(int hour, int min, int sec = 0);
    void GetTime(int &hour, int &min);

    // QQ 音乐代理配置
    void SetMusicProxy(const char* host, int port);
    std::string music_proxy_host_;
    int music_proxy_port_ = 8765;

    bool IsRunning() { return is_running_.load(); }
    void SetDark(bool dark) { is_dark_ = dark; }
    bool CheckDark() { return ldr_ ? ldr_->IsDark() : false; }
};

/**
 * 布谷鸟钟 MCP 工具集
 * 注册所有钟控 MCP 工具到小智 AI
 */
class CuckooTools {
private:
    CuckooStateMachine* state_machine_;

public:
    CuckooTools(CuckooStateMachine* sm);
    void RegisterAll();                        // 注册全部 MCP 工具
};

// 布谷鸟钟时钟任务（FreeRTOS，Core 1，250ms tick）
void cuckoo_clock_task(void* params);

#endif // __CUCKOO_CONTROLLER_H__
