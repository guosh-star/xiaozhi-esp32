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

// MP3 decoder
#include <esp_audio_types.h>
#include <decoder/esp_audio_dec.h>
#include <decoder/impl/esp_mp3_dec.h>

// 布谷鸟唤醒声音数据
#include "cuckoo_wake_sound.h"

// OGG/Opus demuxer for native decode pipeline
#include "ogg_demuxer.h"

// ============================================
// 布谷鸟钟外设控制器
// ============================================

/**
 * 电机驱动封装 (支持 TB6612 / DRV8833)
 *
 * TB6612: 3线控制 (PWMA + AIN1 + AIN2)
 *   - 构造函数: Motor(pwm, in1, in2, channel)
 * DRV8833: 2线控制 (AIN1 + AIN2, PWM打在IN脚上)
 *   - 构造函数: Motor(in1, in2, ch1, ch2)  // ch1=AIN1的LEDC, ch2=AIN2的LEDC
 */
class Motor {
private:
    gpio_num_t in1_pin_;
    gpio_num_t in2_pin_;
    bool use_pwm_;
    // PWM mode fields
    ledc_channel_t ledc_channel_;
    ledc_channel_t ledc_channel2_;
    ledc_timer_t ledc_timer_;
    ledc_mode_t speed_mode_;
    uint32_t max_duty_;  // depends on timer resolution (255 for 8-bit, 1023 for 10-bit)

public:
    // GPIO 直驱模式（水车、舞蹈、大门、小提琴）
    Motor(gpio_num_t in1_pin, gpio_num_t in2_pin);
    // PWM 模式（鸟门、小狗）— 保留旧接口
    Motor(gpio_num_t in1_pin, gpio_num_t in2_pin, ledc_channel_t ch1, ledc_channel_t ch2,
          ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
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
    int angle_;       // 0-180度
    ledc_channel_t ledc_channel_;
    ledc_mode_t speed_mode_;  // LOW_SPEED or HIGH_SPEED

public:
    Servo(gpio_num_t pin, ledc_channel_t channel,
          ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
    void SetAngle(int angle);       // 0-180
    int GetAngle() { return angle_; }
    void Sweep(int from, int to, int duration_ms = 500);  // 渐进摆动
};

/**
 * 板C M2 水车+鸟跳 (通过 DRV8833 单向独立控制，原 BirdJump 类已废弃)
 */
// BirdJump 类已移除，改为 Motor 类控制 (DRV8833 IN2=HIGH -> 鸟跳)
// 详见 cuckoo_controller.cc 中 water_bird_->SetSpeed(-100) 实现


class BellSoundPlayer {
public:
    BellSoundPlayer() = default;
    ~BellSoundPlayer() = default;

    // 初始化总是成功（复用 AI 音频系统）
    bool Init() { return true; }

    // 播放布谷鸟叫声（同步，阻塞直到播放完成）
    void PlayCuckooSoundSync();
    void PlayBellSoundSync();

    // 验证初始化状态
    bool IsInitialized() { return true; }
};

/**
 * PCF8563 RTC 驱动 (I2C)
 * 当前未使用（时间通过 NTP 同步），保留接口备用
 */

/**
 * MP3 软件解码播放器
 * 从 assets 分区读取 MP3 文件，用 esp_mp3_dec 软解码，
 * 通过 AudioService::OutputRawPcm 输出到 I2S 扬声器。
 * 内部使用 FreeRTOS 任务异步播放，MCP 调用不会阻塞。
 */
class Mp3Player {
private:
    Assets* assets_ = nullptr;
    void* mp3_dec_handle_ = nullptr;
    std::atomic<bool> is_playing_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<float> ducking_gain_{1.0f};     // 1.0=normal, 0.0=silent (smooth fade)
    std::atomic<int64_t> ducking_start_us_{0};  // when fading started

    // 播放任务
    TaskHandle_t play_task_ = nullptr;
    int pending_track_ = 0;       // 待播放曲目编号(1-13)，0=无待播
    int pending_bell_hour_ = 0;   // 待播钟声重复次数

    // 解码缓冲区
    static constexpr size_t kInputBufSize = 8192;      // MP3 帧最大大小
    static constexpr size_t kOutputBufSize = 8192;     // PCM 输出缓冲区（1152样本*2字节*2声道）
    uint8_t input_buf_[kInputBufSize];
    uint8_t output_buf_[kOutputBufSize];

    // 钟声播放器（PCM header，短促撞击声）
    BellSoundPlayer bell_player_;

    // 播放任务入口
    static void PlayTaskEntry(void* arg);

    // === Ducking 渐隐公共方法 ===
    // 检测 AI 状态，更新 ducking_gain_，并对 PCM 数据应用增益
    // 所有播放路径统一调用，避免逻辑重复
    void UpdateDuckingState();
    void ApplyDuckingGain(int16_t* pcm, size_t num_samples);

public:
    Mp3Player() = default;
    ~Mp3Player();

    void Init(Assets* assets);

    // 解码单个文件并播放，返回实际播放时长（ms），0=失败或被中断
    int DecodeSingleFile(int index);

    // 解码单个文件到 PCM 缓冲区（不播放），返回样本数和采样率
    // 调用者负责 free() out_buf
    int DecodeToBuffer(int index, int16_t** out_buf, size_t* out_samples, int* out_samplerate);

    void PlayTrack(uint8_t folder, uint8_t track);  // folder=1钟声, folder=2音乐, track=索引
    void PlayIndex(uint16_t index);                  // 直接播放序号（0001.mp3~0013.mp3）
    void Stop();
    void ResetForNextPlay();
    void Pause();
    void Resume();
    void SetVolume(uint8_t vol);
    void VolumeUp();
    void VolumeDown();
    void Next();
    void Prev();
    void PlayBell(int hour);                         // 播放钟声(1-12)，用 0013.mp3 重复播放
    void PlayBgMusic(int index);                     // 播放背景音乐(1-12) -- 0001~0012.mp3
    void PlayAlarmRing(float volume = 1.0f);         // 播放闹钟铃声（0.0~1.0，默认全音量）
    int PlayUrl(const char* url);                    // [已弃用] HTTP下载MP3并播放
    int PlayOpus(const char* url);                   // HTTP下载OGG/Opus -> 原生解码管线
    int PlayPcm(const char* url);                    // HTTP下载raw PCM并走原生播放管线
    static void PlayUrlTask(void* arg);              // [已弃用] PlayUrl后台任务入口
    static void PlayOpusTask(void* arg);             // PlayOpus后台任务入口
    static void PlayPcmTask(void* arg);              // PlayPcm后台任务入口
    bool IsPlaying() { return is_playing_; }
};

/**
 * 鸟叫播放器
 * 通过小智 AI 的 AudioService 输出原始 PCM 数据到板载扬声器
 * 不再使用独立的 I2S 通道（避免与 AI 音频冲突）
 */
// DEPRECATED: Using NTP sync instead of hardware RTC (I2C1 not wired)
private:
    i2c_port_t i2c_port_;
    uint8_t bcd_to_dec(uint8_t bcd);
    uint8_t dec_to_bcd(uint8_t dec);

public:
    RtcPcf8563(i2c_port_t port);
    bool Init();
    bool GetTime(int &year, int &month, int &day, int &hour, int &min, int &sec);
    bool GetHourMin(int &hour, int &min);   // 只读取时和分
};

/**
 * 光敏电阻 (ADC 模拟读取，昼夜检测)
 */
class LdrSensor {
private:
    gpio_num_t adc_pin_;
    adc_oneshot_unit_handle_t adc_handle_;
    adc_channel_t adc_chan_;
    int threshold_;     // 亮度阈值 (0-4095)

public:
    LdrSensor(gpio_num_t adc_pin, adc_unit_t unit, adc_channel_t chan, int threshold = 2000);
    ~LdrSensor();
    int ReadRaw();
    bool IsDark();
    void SetThreshold(int threshold);
};

/**
 * 布谷鸟钟状态机
 * 管理整套表演动作序列
 */
class CuckooStateMachine {
public:
    enum PerformanceType {
        kPerformanceNone,       // 空闲
        kPerformanceHour,       // 整点表演(带钟声)
        kPerformanceHalf,       // 半点表演(简化)
        kPerformanceManual,     // 手动触发
        kPerformanceBgMusic,    // 播放背景音乐
    };

    enum Phase {
        kPhaseIdle,
        kPhaseOpeningDoor,      // 开门
        kPhaseBirdOut,          // 小鸟弹出
        kPhaseCalling,          // 咕咕叫(固定三声)
        kPhaseBirdIn,           // 小鸟回去
        kPhaseClosingDoor,      // 关门
        kPhaseChiming,          // 敲钟(0013.mp3重复整点次数)
        kPhaseDance,            // 跳舞+水车+背景音乐
        kPhaseDone,             // 完成
    };

    // Dedup check for hourly/half-hourly chime (独立去重，避免竞态)
    bool NeedHourlyChime(int hour) const { return hour != last_hour_; }
    bool NeedHalfHourlyChime(int hour) const { return hour != last_half_hour_; }
    void MarkHourlyChime(int hour) { last_hour_ = hour; }
    void MarkHalfHourlyChime(int hour) { last_half_hour_ = hour; }

private:
    Motor* m1_;            // 舞蹈电机 (板A M1)
    Motor* m2_;            // 大门电机 (板A M2)
    Motor* m3_;            // 小狗电机 (板B M1)
    Motor* m4_;            // 鸟门电机 (板C M1)
    Motor* violin_motor_;  // 小提琴电机 (板B M2, GPIO18/45)
    Servo* violin_servo_;  // 小提琴手舵机
    Servo* dog_servo_;     // 小狗摇头舵机
    Motor* water_bird_;    // 板C M2: 水车(IN1) + 鸟跳(IN2), 单向独立控制
    Mp3Player* mp3_;
    BellSoundPlayer* bell_player_;  // 鸟叫播放器（复用AI音频系统）
    // RtcPcf8563* rtc_;  // [DEPRECATED] not wired, using NTP
    LdrSensor* ldr_;
    gpio_num_t motor_power_pin_ = GPIO_NUM_NC;

    PerformanceType current_performance_;
    Phase current_phase_;
    int call_count_;        // 还剩余叫几声
    int total_calls_;       // 总共要叫几声(=整点数)
    std::atomic<bool> is_running_{false};
    int last_hour_;         // 上次整点(防重复)
    int last_half_hour_;    // 上次半点(防重复)
    int show_music_index_;  // 上次Show播放的音乐编号(1-12)

    // 动作循环状态（避免 static 局部变量）
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

    struct DogTailState {
        int angle = 0;
        int dir = 1;
        int target = 50;
        int pause = 0;
        void Reset() { angle = 0; dir = 1; target = 50; pause = 0; }
    };
    DogTailState dog_state_;

public:
    // 内部时钟（钟控任务访问，设为 public）
    std::atomic<int> current_hour_{0};      // 内部小时(0-23)
    std::atomic<int> current_min_{0};       // 内部分钟(0-59)
    std::atomic<int> current_sec_{0};       // 内部秒(0-59)
    std::atomic<bool> time_set_{false};         // 时间是否已设置
    std::atomic<bool> is_dark_{false};          // 夜间模式(晚上不表演)

    // 防AI误触发：记录设备从idle退出的时间戳（钟控任务访问，设为 public）
    uint64_t last_idle_exit_us_ = 0;
    int prev_device_state_ = -1;  // 上一次设备状态（用于检测idle->active转换）

    // === 闹钟功能 ===
    static constexpr int kMaxAlarms = 5;

    struct AlarmInfo {
        int hour;
        int minute;
        bool enabled;
        bool repeat_daily;  // true=每天重复, false=单次（响过后自动关）
    };

    mutable std::mutex alarm_mutex_;
    AlarmInfo alarms_[kMaxAlarms] = {};
    std::atomic<int> alarm_count_{0};
    std::atomic<bool> alarm_ringing_{false};
    std::atomic<bool> alarm_stopped_{false};

    void SetAlarm(int hour, int minute, bool repeat_daily);
    std::string GetAlarmsJson();
    bool DeleteAlarm(int index);  // 1-based index
    bool IsAlarmRinging() const { return alarm_ringing_; }
    
    // Alarm persistence (NVS-backed, survives reboot) — 暂未实现
    // void SaveAlarmsToNvs();  // TODO: implement NVS backup for alarms
    // void LoadAlarmsFromNvs();  // TODO: implement NVS restore for alarms
    void StopAlarm();
    void CheckAlarms(int hour, int minute, int sec);
    static void AlarmTask(void* arg);

    CuckooStateMachine(Motor* m1, Motor* m2, Motor* m3, Motor* m4,
                       Motor* violin_motor, Servo* violin, Servo* dog,
                       Motor* water_bird, Mp3Player* mp3,
                       BellSoundPlayer* bell_player,
                       // RtcPcf8563* rtc,  // [DEPRECATED] using NTP
                       LdrSensor* ldr);

    // 电机驱动电源控制
    void MotorPowerOn();
    void MotorPowerOff();

    // 一次性表演（非阻塞，在独立任务中执行）
    void StartPerformance(PerformanceType type, int hour = 0);

    // 查询是否正在播放音乐（用于后台判断是否保持 WiFi 高性能）
    bool IsMusicPlaying() { return mp3_ && mp3_->IsPlaying(); }
    void StopMusic();

    // 内部：表演执行任务（线性执行所有阶段，无竞态问题）
    static void PerformanceTask(void* arg);

    // 整点/半点检测
    void CheckTime(int hour, int min, bool dark);

    // 控制命令
    void Dance();           // 单独跳舞
    void StartShow();       // 表演开始(跳舞+水车+音乐, 在ShowTask中线性执行)
    static void ShowTask(void* arg);  // StartShow的线性执行任务
    void PlayMusic(int index);  // 播放背景音乐
    void StopAll();         // 紧急停止
    void OpenDoor();        // 单独开门
    void CloseDoor();       // 单独关门
    void BirdJumpOnce();    // 小鸟跳一下
    void OpenBirdDoor();    // 打开鸟门（唤醒AI时）
    void CloseBirdDoor();   // 关闭鸟门（AI休眠时）
    void PlayCuckooSound(); // 播放布谷鸟叫声
    void BirdJumpPulse();   // 小鸟脉冲跳（说话时用）
    static void AutoCloseTimerCallback(TimerHandle_t timer);  // 鸟门自动关闭定时器回调
    void SetServoAngle(int servo_id, int angle);  // 舵机控制 0=小提琴,1=小狗
    void SetMotorSpeed(int motor_id, int speed);   // 电机控制 1-4
    void SetBirdDoorSpeed(int speed);              // 鸟门电机

    // 在线音乐播放 (通过中转服务)
    int PlayOnlineMusic(const char* url);           // 在后台下载URL音频并播放，立即返回

    struct MusicTaskCtx {
        CuckooStateMachine* sm;
        char url[512];
    };
    static void PlayOnlineMusicTask(void* arg);  // 后台播放任务

    // 设置内部时间（由 MCP cuckoo.set_time 调用，或由服务器同步时调用）
    void SetTime(int hour, int min, int sec = 0);
    // 获取当前内部时间
    void GetTime(int &hour, int &min);

    // 在线音乐代理地址（运行时配置）
    void SetMusicProxy(const char* host, int port);
    std::string music_proxy_host_;  // 代理地址
    int music_proxy_port_ = 8765;   // 代理端口

    bool IsRunning() { return is_running_.load(); }
    void SetDark(bool dark) { is_dark_ = dark; }
    bool CheckDark() { return ldr_ ? ldr_->IsDark() : false; }
};

/**
 * 布谷鸟钟 MCP 工具集
 * 注册所有钟控 MCP 工具到小智
 */
class CuckooTools {
private:
    CuckooStateMachine* state_machine_;

public:
    CuckooTools(CuckooStateMachine* sm);
    void RegisterAll();  // 注册所有MCP工具
};

// 钟控FreeRTOS任务 - 运行在Core 1
void cuckoo_clock_task(void* params);

#endif // __CUCKOO_CONTROLLER_H__
