#ifndef __CUCKOO_CONTROLLER_H__
#define __CUCKOO_CONTROLLER_H__

#include "cuckoo_action_gate.h"

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
 * @brief 2克微型舵机封装
 * 使用 PWM 控制，50Hz，0.5-2.5ms 脉冲，角度 0~145°（上限钳位，避免 DS-M005 顶死）
 */
class Servo {
private:
    gpio_num_t pin_;
    int angle_;                              // 当前角度 0°~145°
    ledc_channel_t ledc_channel_;
    ledc_mode_t speed_mode_;                 // LOW_SPEED or HIGH_SPEED

public:
    Servo(gpio_num_t pin, ledc_channel_t channel,
          ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE);
    void SetAngle(int angle);                // 设置 0°~145°
    int GetAngle() { return angle_; }
    void Sweep(int from, int to, int duration_ms = 500); // 平滑扫过角度
    void Release();                          // 归零释放：duty=0 不锁力
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
 * @brief 钟声合成播放器
 * 播放布谷鸟叫声（同步/异步）与钟声音效，支持独立初始化。
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
    std::atomic<int> pending_track_{0};              // 待播放曲目
    std::atomic<int> pending_bell_hour_{0};          // 待播放报时小时数

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
    // Immutable local-file/chime jobs; no persistent worker can re-read a new request.
    mutable std::mutex offline_lifetime_mutex_;
    std::condition_variable offline_drained_;
    size_t offline_jobs_ = 0;
    bool offline_closing_ = false;
    bool OfflineClosing() const { std::lock_guard<std::mutex> lock(offline_lifetime_mutex_); return offline_closing_; }
    struct OfflineJob {
        Mp3Player* player;
        uint64_t sequence;
        cuckoo_stop::Ticket ticket;
        int track, hour;
        bool background;
        std::function<bool()> keep, audio_allowed;
        std::function<void()> retired;
        std::shared_ptr<void> lifetime;
    };
    std::atomic<uint64_t> offline_generation_{0};
    std::atomic<int> offline_kind_{0}; // 0=none, 1=chime, 2=accepted background
    bool OfflineCurrent(const OfflineJob& job) const;
    bool QueueOffline(int track, int hour, bool background,
                      std::function<bool()> keep = {}, std::function<void()> retired = {},
                      std::function<bool()> audio_allowed = {});
    bool PlayBackground(int index, std::function<bool()> keep,
                        std::function<void()> retired, std::function<bool()> audio_allowed) {
        return QueueOffline(index < 1 ? 1 : index, 0, true,
                            std::move(keep), std::move(retired), std::move(audio_allowed));
    }
    bool HasBackgroundPlayback() const { return offline_kind_.load() == 2; }
    bool HasChimePlayback() const { return offline_kind_.load() == 1; }

    Mp3Player() = default;
    ~Mp3Player();

    void Init(Assets* assets);

    // 解码并播放单个 MP3 文件
    int DecodeSingleFile(int index, std::function<bool()> keep_running = {}, std::function<bool(const int16_t*, size_t, int)> output = {});

    // 解码 MP3 到内存缓冲区（不直接播放）
    int DecodeToBuffer(int index, int16_t** out_buf, size_t* out_samples, int* out_samplerate);

    // 流式解码 MP3，逐帧回调（零大 buffer，适合长音频）
    int DecodeStreaming(int index, std::function<void(const int16_t* pcm, size_t samples, int src_sr, void* user_data)> callback, void* user_data = nullptr, std::function<bool()> keep_running = {});

    void PlayTrack(uint8_t folder, uint8_t track); // 播放指定曲目
    void PlayIndex(uint16_t index);                // 按全局索引播放
    void Stop();
    void RequestStop() {
        cuckoo_stop::Gate::Instance().MediaState([&] {
            stop_requested_ = true; ++offline_generation_;
            if (offline_kind_.exchange(0) != 0) is_playing_ = false;
            pending_track_ = 0; pending_bell_hour_ = 0;
        });
    }
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
 * @brief 光敏电阻传感器（ADC 模拟读取，昼夜检测）
 * 用 ADC oneshot 读取环境亮度，阈值以下判定为暗（夜间静音模式用）。
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

    cuckoo_stop::Value<PerformanceType> current_performance_;
    cuckoo_stop::Value<Phase> current_phase_;
    cuckoo_stop::Value<int> call_count_;                           // 布谷鸟叫声计数
    cuckoo_stop::Value<int> total_calls_;                          // 布谷鸟叫声总次数
    cuckoo_stop::Value<bool> is_running_{false};
    int last_hour_;                            // 上次整点报时的小时
    int last_half_hour_;                       // 上次半点报时的小时
    cuckoo_stop::Value<int> show_music_index_;                   // 当前播放曲目编号
    cuckoo_stop::Value<int> music_dance_phase_ = 0;              // 音乐舞蹈相位 (0-7)
    cuckoo_stop::Value<int> music_dance_enabled_ = -1;           // 音乐舞蹈状态: -1=未初始化, 0=已停止, 1=激活中
    cuckoo_stop::Value<int> m1_music_fwd_count_ = 0;               // 音乐中 M1 正转脉冲计数
    cuckoo_stop::Value<int> m1_music_rev_count_ = 0;               // 音乐中 M1 反转脉冲计数
    cuckoo_stop::Value<bool> dog_outro_done_ = false;
    cuckoo_stop::Value<bool> dog_outro_running_{false};  // 小狗回家动画进行中
    cuckoo_stop::Value<bool> dog_intro_running_{false};  // 小狗出场动画进行中（防重复创建）
    cuckoo_stop::Value<bool> dog_intro_done_{false};     // 小狗出场完成
    cuckoo_stop::Value<bool> kids_active_{true};         // Kids 模式是否激活（默认true，启动即活跃）
    cuckoo_stop::Value<bool> kids_dancing_{false};       // 小朋友跳舞模式（false=安静欣赏, true=电机舞蹈）
    cuckoo_stop::Value<bool> kids_appreciating_{true};    // 轻摆欣赏模式（M1 MusicDanceTick 相位脉冲）默认true
    cuckoo_stop::Value<bool> flash_leds_active_{false};   // LED 闪烁模式是否激活

    // 小提琴循环状态
    struct ViolinLoopState {
        cuckoo_stop::Value<int> stage = 0;
        cuckoo_stop::Value<int> loop_count = 0;
        cuckoo_stop::Value<int> timer = 0;
        cuckoo_stop::Value<int> angle = 90;
        cuckoo_stop::Value<int> fwd_count = 0;
        cuckoo_stop::Value<int> rev_count = 0;
        void Reset() { stage = 0; loop_count = 0; timer = 0; angle = 90; fwd_count = 0; rev_count = 0; }
    };
    ViolinLoopState violin_state_;

    // 狗尾摇摆状态
    struct DogTailState {
        cuckoo_stop::Value<int> angle = 15;
        cuckoo_stop::Value<int> dir = 1;
        cuckoo_stop::Value<int> target = 65;
        cuckoo_stop::Value<int> pause = 0;
        void Reset() { angle = 15; dir = 1; target = 65; pause = 0; }
    };
    DogTailState dog_state_;
    cuckoo_stop::Value<bool> door_open_{false};   // 大门状态
    cuckoo_stop::Value<bool> dog_out_{false};    // 小狗状态
    cuckoo_stop::Value<bool> kids_dance_{false}; // KidsDanceShow 触发标志

    // M1 舞蹈电机时序
    cuckoo_stop::Value<unsigned long> m1_fwd_time_ = 0;
    cuckoo_stop::Value<unsigned long> m1_rev_time_ = 0;
    cuckoo_stop::Value<unsigned long> m1_stage_start_ = 0;

public:
    // 时间状态
    std::atomic<int> current_hour_{0};         // 当前小时
    std::atomic<int> current_min_{0};          // 当前分钟
    std::atomic<int> current_sec_{0};          // 当前秒
    std::atomic<bool> time_set_{false};        // 时间是否已通过 NTP 设定
    std::atomic<bool> is_dark_{false};         // 当前是否暗光

    // 唤醒时间戳
    cuckoo_stop::Value<uint64_t> last_idle_exit_us_ = 0;
    cuckoo_stop::Value<int> prev_device_state_ = -1;               // 上一次设备状态

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
    cuckoo_stop::Value<bool> alarm_ringing_{false};
    cuckoo_stop::Value<bool> alarm_stopped_{false};
    cuckoo_stop::Value<int> thanks_counter_{0};       // 感谢语计数
    cuckoo_stop::Value<int> linda_song_counter_{0};   // 琳达歌曲计数
    cuckoo_stop::Value<int> garden_song_counter_{0};  // 花园歌曲计数

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

    // 最近一次成功播放的在线歌曲（idle/重启后仍可重唱）
    std::string last_song_name_;
    std::string last_song_url_;
    void SaveLastSong(const std::string& name, const std::string& url);
    void LoadLastSong();

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
    void ReleaseServos();   // 空闲归零：释放两个舵机（duty=0）

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
    void StartFlashLeds();
    void StopFlashLeds();
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
    int PlayOnlineMusicNow(const char* url, uint64_t expected_request = 0);   // 立即播放（跳过“等 AI 说完”排队）
    static void MusicDeferTask(void* arg);     // 等 TTS 结束后再开始播放
    std::string music_pending_url_;
    std::mutex music_pending_mutex_;
    TaskHandle_t music_defer_task_ = nullptr;
    std::string IdentifySong();
    std::string CheckMultiArtist(const char* url_or_path);
    /// 查询在线音乐账号状态（会员/订阅/cookie 是否有效），返回服务端 JSON 摘要
    std::string GetMusicAccountStatus();
    // 云端曲库/歌单
    std::string LibrarySaveLast(const std::string& playlist);
    std::string LibraryListPlaylists();
    std::string LibraryPlaylistInfo(const std::string& playlist);
    std::string LibraryDeletePlaylist(const std::string& playlist);
    std::string LibraryCopyTrack(const std::string& src, int index,
                                 const std::string& dst);
    std::string LibraryRenamePlaylist(const std::string& old_name,
                                      const std::string& new_name);
    std::string LibraryUndoDeletePlaylist(const std::string& playlist);
    std::string LibraryRemoveFromPlaylist(const std::string& playlist,
                                          const std::string& song,
                                          const std::string& artist,
                                          int index);
    int PlayPlaylist(const std::string& playlist);
    int PlayPlaylistTrack(const std::string& playlist, int index);

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

    // Single-board native S3 action ownership. Scalar commits share the output fence.
    cuckoo_stop::Value<bool> motion_suspended_{false};
    std::atomic<uint64_t> media_generation_{0};
    std::atomic<uint64_t> pending_music_generation_{0};
    std::atomic<bool> background_owned_{false};
    std::atomic<bool> background_streaming_{false};
    std::atomic<uint64_t> active_background_token_{0};
    bool PreserveBackground() const {
        return mp3_ && (mp3_->HasBackgroundPlayback() ||
            (background_owned_.load() && active_background_token_.load() == media_generation_.load() &&
             active_background_token_.load() != 0 && !mp3_->HasChimePlayback() &&
             (background_streaming_.load() || mp3_->IsPlaying())));
    }
    void RetireMotion();
    void FailPerformanceStart();
    bool ClaimPerformance(PerformanceType type) {
        if (is_running_.load()) return false;
        RetireMotion();
        alarm_stopped_ = true; alarm_ringing_ = false;
        if (mp3_ && !PreserveBackground()) mp3_->RequestStop();
        is_running_ = true; current_performance_ = type;
        motion_suspended_ = false;
        return true;
    }
    bool MotionSuppressed() const {
        return motion_suspended_.load() && cuckoo_stop::Gate::Instance().Automatic();
    }
    void ResumeMotionForCommand() {
        if (!cuckoo_stop::Gate::Instance().Automatic()) motion_suspended_ = false;
    }
    cuckoo_stop::Value<uint64_t> performance_media_ticket_{0}, performance_media_generation_{0};
    cuckoo_stop::Value<bool> performance_owns_background_{false};
    void OwnPerformanceMedia(bool reserve_background = true) {
        cuckoo_stop::Gate::Instance().Write([&] {
            performance_owns_background_ = reserve_background;
            performance_media_ticket_ = cuckoo_stop::Gate::Instance().CurrentTicket();
            performance_media_generation_ = reserve_background ? media_generation_.fetch_add(1) + 1 : media_generation_.load();
        });
    }
    template<class F> bool PerformanceMedia(F&& f) {
        bool done = false;
        cuckoo_stop::Gate::Instance().Media([&] {
            if (performance_media_ticket_.load() == cuckoo_stop::Gate::Instance().CurrentTicket()) { f(); done = true; }
        });
        return done;
    }
    template<class F> bool PerformanceTrack(F&& f) {
        bool done = false;
        cuckoo_stop::Gate::Instance().Media([&] {
            if (performance_media_ticket_.load() == cuckoo_stop::Gate::Instance().CurrentTicket() &&
                performance_media_generation_.load() == media_generation_.load()) { f(); done = true; }
        });
        return done;
    }
    void FinishPerformanceMedia();
    uint64_t BeginBackground(bool performance = false, bool revoke_pending = true) {
        uint64_t token = 0;
        cuckoo_stop::Gate::Instance().Media([&] {
            cuckoo_stop::Gate::Instance().Write([&] {
                if (performance) {
                    if (performance_media_ticket_.load() != cuckoo_stop::Gate::Instance().CurrentTicket() ||
                        performance_media_generation_.load() != media_generation_.load()) return;
                    token = performance_media_generation_.load();
                } else {
                    if (revoke_pending) ++pending_music_generation_;
                    token = media_generation_.fetch_add(1) + 1;
                }
                active_background_token_ = token;
                background_owned_ = true; background_streaming_ = false;
            });
        });
        return token;
    }
    bool MediaCurrent(uint64_t token) const {
        return token && token == media_generation_.load() && !cuckoo_stop::Gate::Instance().Closing();
    }
    template<class F> bool MediaChunk(uint64_t token, F&& f) {
        bool done = false;
        cuckoo_stop::Gate::Instance().MediaOwner([&] {
            if (MediaCurrent(token)) { f(); done = true; }
        });
        return done;
    }

    bool IsRunning() { return is_running_.load(); }
    void SetDark(bool dark) { is_dark_ = dark; }
    bool CheckDark() { return ldr_ ? ldr_->IsDark() : false; }
};

/**
 * @brief 布谷鸟钟 MCP 工具集
 * 注册所有钟控 MCP 工具到小智 AI（报时、演出、音乐、闹钟、门/灯等）。
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
