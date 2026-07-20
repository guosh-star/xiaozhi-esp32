#include "wifi_board.h"
#include "../../audio/codecs/box_audio_codec.h"
#include "../../display/oled_display.h"
#include "../common/system_reset.h"
#include "../../application.h"
#include "../common/button.h"
#include "config.h"
#include "../../mcp_server.h"
#include "../../led/ws2812_led.h"
#include "../../assets/lang_config.h"

#include "cuckoo_controller.h"
#include "assets.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "CuckooBoard"

// ============================================
// �������Ӱ��� - �̳� WifiBoard
// ============================================
class CuckooBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;

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



    // ����ָ�루δ����ʱΪ nullptr��״̬����������
    Motor* m1_ = nullptr;              // �赸��� (��A M1)
    Motor* m2_ = nullptr;              // ���ŵ�� (��A M2)
    Motor* m3_ = nullptr;              // С����� (��B M1)
    Motor* m4_ = nullptr;              // 鸟门电机 (板C M1)
    Motor* bird_door_motor_ = nullptr; // ⚠️ 变量名历史遗留，实际是【小提琴电机】(板B M2, GPIO18/45)
    Servo* violin_servo_ = nullptr;    // 小提琴手舵机
    Servo* dog_servo_ = nullptr;       // С��ҡͷ���
    Motor* water_bird_ = nullptr;          // ��C M2: ˮ��(IN1/GPIO8) + ����(IN2/GPIO39)
    Mp3Player* mp3_ = nullptr;             // MP3���ⲥ����
    BellSoundPlayer* bell_player_ = nullptr;  // ��в�����������AI��Ƶ��
    RtcPcf8563* rtc_ = nullptr;        // RTCʱ��
    LdrSensor* ldr_ = nullptr;         // �������
    CuckooStateMachine* state_machine_ = nullptr;
    CuckooTools* cuckoo_tools_ = nullptr;

    void InitializePeripherals() {
        ESP_LOGI(TAG, "Initializing cuckoo clock peripherals...");

        // 板A M1: 舞蹈电机 GPIO 直驱 (GPIO4/5)
        m1_ = new Motor(MOTOR_DANCE_IN1, MOTOR_DANCE_IN2);
        ESP_LOGI(TAG, "M1 dance ready (GPIO %d/%d, GPIO)", MOTOR_DANCE_IN1, MOTOR_DANCE_IN2);

        // 板A M2: 大门电机 GPIO 直驱 (GPIO6/7)
        m2_ = new Motor(MOTOR_BIRD_IN1, MOTOR_BIRD_IN2);
        ESP_LOGI(TAG, "M2 door ready (GPIO %d/%d, GPIO)", MOTOR_BIRD_IN1, MOTOR_BIRD_IN2);

        // 板B M1: 小狗电机 PWM (GPIO10/11, LEDC timer0 ch4/5)
        m3_ = new Motor(MOTOR_DOG_IN1, MOTOR_DOG_IN2, LEDC_CH_DOG_IN1, LEDC_CH_DOG_IN2);
        ESP_LOGI(TAG, "M3 dog ready (GPIO %d/%d, ch%d/%d)",
                 MOTOR_DOG_IN1, MOTOR_DOG_IN2, LEDC_CH_DOG_IN1, LEDC_CH_DOG_IN2);

        // 板B M2: 小提琴电机 GPIO 直驱 (GPIO18/45)
        bird_door_motor_ = new Motor(MOTOR_VIOLIN_IN1, MOTOR_VIOLIN_IN2);
        ESP_LOGI(TAG, "M4 violin ready (GPIO %d/%d, GPIO)", MOTOR_VIOLIN_IN1, MOTOR_VIOLIN_IN2);

        // 板C M1: 鸟门电机 PWM (GPIO9/46, LEDC timer2 ch2/3)
        m4_ = new Motor(MOTOR_DOOR_IN1, MOTOR_DOOR_IN2, LEDC_CH_DOOR_IN1, LEDC_CH_DOOR_IN2, LEDC_TIMER_DOOR, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "M5 bird door ready (GPIO %d/%d, timer2 ch%d/%d)",
                 MOTOR_DOOR_IN1, MOTOR_DOOR_IN2, LEDC_CH_DOOR_IN1, LEDC_CH_DOOR_IN2);

        // ��� �� HIGH_SPEED ����ͨ��
        violin_servo_ = new Servo(SERVO_VIOLIN, LEDC_CH_SERVO_V, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "Violin servo ready (GPIO %d, ch%d)", SERVO_VIOLIN, LEDC_CH_SERVO_V);
        dog_servo_ = new Servo(SERVO_DOG, LEDC_CH_SERVO_D, LEDC_LOW_SPEED_MODE);
        ESP_LOGI(TAG, "Dog servo ready (GPIO %d, ch%d)", SERVO_DOG, LEDC_CH_SERVO_D);

        // ��в�����
        bell_player_ = new BellSoundPlayer();
        bell_player_->Init();
        ESP_LOGI(TAG, "Bell sound player ready");

        // MP3 ���ⲥ����
        mp3_ = new Mp3Player();
        mp3_->Init(&Assets::GetInstance());
        ESP_LOGI(TAG, "MP3 soft decoder ready");

                // ��C M2: ˮ�� + С����Ծ (DRV8833, �������, Timer2 HIGH_SPEED)
        water_bird_ = new Motor(MOTOR_WATER_BIRD_IN1, MOTOR_WATER_BIRD_IN2);
        ESP_LOGI(TAG, "Water+bird motor ready (GPIO %d/%d, GPIO)", MOTOR_WATER_BIRD_IN1, MOTOR_WATER_BIRD_IN2);

        // ���������� (δ���ߣ��ݲ���ʼ��)
        ldr_ = new LdrSensor(LDR_GPIO, LDR_ADC_UNIT, LDR_ADC_CHANNEL, LDR_DARK);

        ESP_LOGI(TAG, "Peripherals initialized");
    }

    void InitializeStateMachine() {
        state_machine_ = new CuckooStateMachine(
            m1_, m2_, m3_, m4_,
            bird_door_motor_, violin_servo_, dog_servo_,
            water_bird_, mp3_,
            bell_player_,
            rtc_, ldr_
        );
        ESP_LOGI(TAG, "State machine created");
    }

    void InitializeMCPTools() {
        cuckoo_tools_ = new CuckooTools(state_machine_);
        cuckoo_tools_->RegisterAll();
        ESP_LOGI(TAG, "MCP tools registered");
    }

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
                // 鸟门保持打开，等AI对话结束后自行关闭
            }
            app.ToggleChatState();
        });

        // ���� BOOT �� 1 �� = ����ֹͣ��ֹͣ���е�������֡�AI�����
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
            AUDIO_INPUT_REFERENCE
        );
        return &audio_codec;
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
};

DECLARE_BOARD(CuckooBoard);
