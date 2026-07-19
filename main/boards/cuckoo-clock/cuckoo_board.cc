#include "wifi_board.h"
#include "../../audio/codecs/box_audio_codec.h"
#include "../common/system_reset.h"
#include "../../application.h"
#include "../common/button.h"
#include "config.h"
#include "../../mcp_server.h"
#include "../../led/ws2812_led.h"
#include "../../assets/lang_config.h"

#include "cuckoo_controller.h"
#include "assets.h"
#include "display/display.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "CuckooBoard"

// ============================================
// 布谷鸟钟主板驱动类
// ============================================

// CuckooBoard：ESP32 布谷鸟钟主板，继承 WifiBoard
//
// 管理所有硬件：音频 Codec、4 路电机、2 路舵机、MP3 播放器、钟声、光敏
class CuckooBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;

    // 初始化 ES8311 + ES7210 音频 Codec 的 I2C 总线
    void InitializeCodecI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &codec_i2c_bus_));
    }

    // 外设对象指针
    Motor* m1_ = nullptr;              // M1 舞蹈电机
    Motor* m2_ = nullptr;              // M2 大门电机
    Motor* m3_ = nullptr;              // M3 小狗行走电机
    Motor* m4_ = nullptr;              // M4 小鸟门电机
    Motor* violin_motor_ = nullptr;    // 小提琴升降电机
    Servo* violin_servo_ = nullptr;    // 小提琴手臂舵机
    Servo* dog_servo_ = nullptr;       // 狗尾舵机
    Motor* water_bird_ = nullptr;      // 水车+鸟跳电机
    Mp3Player* mp3_ = nullptr;         // MP3 解码播放器
    BellSoundPlayer* bell_player_ = nullptr; // 钟声播放器
    // 传感器与状态管理
    LdrSensor* ldr_ = nullptr;         // 光敏电阻
    CuckooStateMachine* state_machine_ = nullptr;
    CuckooTools* cuckoo_tools_ = nullptr;

    // 创建所有电机、舵机、MP3、钟声、光敏对象
    void InitializePeripherals() {
        ESP_LOGI(TAG, "Initializing cuckoo clock peripherals...");

        // M1 舞蹈电机（GPIO 直驱）
        m1_ = new Motor(MOTOR_DANCE_IN1, MOTOR_DANCE_IN2);
        ESP_LOGI(TAG, "M1 dance ready (GPIO %d/%d, GPIO)", MOTOR_DANCE_IN1, MOTOR_DANCE_IN2);

        // M2 大门电机（GPIO 直驱）
        m2_ = new Motor(MOTOR_BIRD_IN1, MOTOR_BIRD_IN2);
        ESP_LOGI(TAG, "M2 door ready (GPIO %d/%d, GPIO)", MOTOR_BIRD_IN1, MOTOR_BIRD_IN2);

        // M3 小狗电机（PWM 驱动）
        m3_ = new Motor(MOTOR_DOG_IN1, MOTOR_DOG_IN2, LEDC_CH_DOG_IN1, LEDC_CH_DOG_IN2);
        ESP_LOGI(TAG, "M3 dog ready (GPIO %d/%d, ch%d/%d)",
                 MOTOR_DOG_IN1, MOTOR_DOG_IN2, LEDC_CH_DOG_IN1, LEDC_CH_DOG_IN2);

        // 小提琴升降电机（GPIO 直驱）
        violin_motor_ = new Motor(MOTOR_VIOLIN_IN1, MOTOR_VIOLIN_IN2);
        ESP_LOGI(TAG, "M2 violin motor ready (GPIO %d/%d, GPIO)", MOTOR_VIOLIN_IN1, MOTOR_VIOLIN_IN2);

        // 小鸟门电机（PWM 驱动，定时器2）
        m4_ = new Motor(MOTOR_DOOR_IN1, MOTOR_DOOR_IN2, LEDC_CH_DOOR_IN1, LEDC_CH_DOOR_IN2, LEDC_TIMER_DOOR, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "M5 bird door ready (GPIO %d/%d, timer2 ch%d/%d)",
                 MOTOR_DOOR_IN1, MOTOR_DOOR_IN2, LEDC_CH_DOOR_IN1, LEDC_CH_DOOR_IN2);

        // 小提琴舵机 + 狗尾舵机
        violin_servo_ = new Servo(SERVO_VIOLIN, LEDC_CH_SERVO_V, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "Violin servo ready (GPIO %d, ch%d)", SERVO_VIOLIN, LEDC_CH_SERVO_V);
        dog_servo_ = new Servo(SERVO_DOG, LEDC_CH_SERVO_D, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "Dog servo ready (GPIO %d, ch%d)", SERVO_DOG, LEDC_CH_SERVO_D);

        // 钟声合成播放器
        bell_player_ = new BellSoundPlayer();
        bell_player_->Init();
        ESP_LOGI(TAG, "Bell sound player ready");

        // MP3 软件解码器
        mp3_ = new Mp3Player();
        mp3_->Init(&Assets::GetInstance());
        ESP_LOGI(TAG, "MP3 soft decoder ready");

        // 水车+鸟跳电机（GPIO 直驱）
        water_bird_ = new Motor(MOTOR_WATER_BIRD_IN1, MOTOR_WATER_BIRD_IN2);
        ESP_LOGI(TAG, "Water+bird motor ready (GPIO %d/%d, GPIO)", MOTOR_WATER_BIRD_IN1, MOTOR_WATER_BIRD_IN2);

        // 光敏电阻传感器
        ldr_ = new LdrSensor(LDR_GPIO, LDR_ADC_UNIT, LDR_ADC_CHANNEL, LDR_DARK);

        ESP_LOGI(TAG, "Peripherals initialized");
    }

    // 创建状态机，传入所有外设引用
    void InitializeStateMachine() {
        state_machine_ = new CuckooStateMachine(
            m1_, m2_, m3_, m4_,
            violin_motor_, violin_servo_, dog_servo_,
            water_bird_, mp3_,
            bell_player_,
            ldr_
        );
        ESP_LOGI(TAG, "State machine created");
    }

    // 通过 MCP 服务器注册所有 AI 控制工具
    void InitializeMCPTools() {
        cuckoo_tools_ = new CuckooTools(state_machine_);
        cuckoo_tools_->RegisterAll();
        ESP_LOGI(TAG, "MCP tools registered");
    }

    // 在 Core 1 上启动时钟任务（250ms tick）
    void StartClockTask() {
        TaskHandle_t task_handle;
        xTaskCreatePinnedToCore(
            cuckoo_clock_task,
            "cuckoo_clock",
            8192,
            state_machine_,
            3,
            &task_handle,
            1
        );
        ESP_LOGI(TAG, "Clock task started on core 1");
    }

    // 按键初始化
    // BOOT按键：切换对话 + 报时。TOUCH按键：触发演出
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (state_machine_) {
                state_machine_->OpenBirdDoor();
                state_machine_->BirdJumpPulse();
                state_machine_->PlayCuckooSound();
                state_machine_->BirdJumpPulse();
                state_machine_->PlayCuckooSound();
                // 布谷鸟叫完成，继续切换对话状态
            }
            app.ToggleChatState();
        });

        // BOOT 长按：紧急停止所有演出
        boot_button_.OnLongPress([this]() {
            if (state_machine_) {
                ESP_LOGI(TAG, "BOOT long press: emergency stop");
                state_machine_->StopAll();
            }
        });

        touch_button_.OnClick([this]() {
            if (state_machine_) state_machine_->StartShow();
        });
    }

public:
    CuckooBoard() : 
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO) {
        
        ESP_LOGI(TAG, "=== Cuckoo Clock Board ===");
        
        InitializeCodecI2c();
        display_ = new NoDisplay();
        InitializePeripherals();
        InitializeStateMachine();
        InitializeMCPTools();
        InitializeButtons();
        StartClockTask();

        ESP_LOGI(TAG, "Cuckoo board ready! Core 0: AI voice, Core 1: Clock control");
    }

    virtual Led* GetLed() override {
        static Ws2812Led led(GPIO_NUM_48);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            codec_i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE,
            42.0f  // input_gain：提升麦克风灵敏度改善唤醒率
        );
        return &audio_codec;
    }

    // 覆盖默认 input_gain（30→42），提升麦克风灵敏度改善唤醒率
    virtual AudioCodec* GetAudioCodec() override = delete;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual void StopAlarm() override {
        if (state_machine_ && state_machine_->IsAlarmRinging()) {
            state_machine_->StopAlarm();
            ESP_LOGI(TAG, "Alarm stopped on wake-up");
        }
    }

    virtual bool IsAlarmRinging() override {
        return state_machine_ ? state_machine_->IsAlarmRinging() : false;
    }

    // 省电模式控制：有后台音频播放时保持性能模式
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::LOW_POWER) {
            auto& audio = Application::GetInstance().GetAudioService();
            if (audio.IsBgAudioActive()) {
                // 后台音频播放中，保持性能模式避免卡顿
                WifiBoard::SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                return;
            }
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(CuckooBoard);
