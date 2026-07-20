#include "wifi_board.h"
#include "../../audio/codecs/box_audio_codec.h"
#include "../../display/oled_display.h"
#include "../common/system_reset.h"
#include "../../application.h"
#include "../common/button.h"
#include "config.h"
#include "../../mcp_server.h"
#include "../../led/single_led.h"
#include "../../assets/lang_config.h"

#include "cuckoo_controller.h"
#include "assets.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_wifi.h>

#define TAG "CuckooBoard"

// ============================================
// 布谷鸟钟板型 - 继承 WifiBoard
// ============================================
class CuckooBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    Button boot_button_;
    Button touch_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = 0x3C,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .scl_speed_hz = 100 * 1000,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(display_i2c_bus_, &io_config, &panel_io_));
        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = -1;
        panel_config.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    // 外设指针（未接线时为 nullptr，状态机会跳过）
    Motor* m1_ = nullptr;              // 舞蹈电机
    Motor* m2_ = nullptr;              // 小提琴电机
    Motor* m3_ = nullptr;              // 水车电机
    Motor* m4_ = nullptr;              // 大门电机
    Motor* bird_door_motor_ = nullptr; // 鸟门电机
    Servo* violin_servo_ = nullptr;    // 小提琴手舵机
    Servo* dog_servo_ = nullptr;       // 小狗摇头舵机
    BirdJump* bird_jump_ = nullptr;    // 小鸟跳跃电磁铁
    Mp3Player* mp3_ = nullptr;             // MP3软解播放器
    BellSoundPlayer* bell_player_ = nullptr;  // 鸟叫播放器（复用AI音频）
    RtcPcf8563* rtc_ = nullptr;        // RTC时钟
    LdrSensor* ldr_ = nullptr;         // 光敏检测
    CuckooStateMachine* state_machine_ = nullptr;
    CuckooTools* cuckoo_tools_ = nullptr;

    void InitializePeripherals() {
        ESP_LOGI(TAG, "Initializing cuckoo clock peripherals...");

        // WS2812 数据脚拉低，防止浮空随机闪烁（锂电池场景省电）
        gpio_config_t ws2812_cfg = {
            .pin_bit_mask = (1ULL << GPIO_NUM_48),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        gpio_config(&ws2812_cfg);
        gpio_set_level(GPIO_NUM_48, 0);

        // 舞蹈电机 M1 (DRV8833: IN1/IN2 → OUT1/OUT2)
        m1_ = new Motor(DRV8833_M1_IN1, DRV8833_M1_IN2, LEDC_CH_M1_AIN1, LEDC_CH_M1_AIN2);
        ESP_LOGI(TAG, "M1 dance motor ready (DRV8833 IN1/IN2: GPIO %d/%d, ch%d/%d)",
                 DRV8833_M1_IN1, DRV8833_M1_IN2, LEDC_CH_M1_AIN1, LEDC_CH_M1_AIN2);

        // 小提琴电机 M2 (DRV8833 同一模块: IN3/IN4 → OUT3/OUT4)
        m2_ = new Motor(DRV8833_M2_IN3, DRV8833_M2_IN4, LEDC_CH_M2_AIN1, LEDC_CH_M2_AIN2);
        ESP_LOGI(TAG, "M2 violin motor ready (DRV8833 IN3/IN4: GPIO %d/%d, ch%d/%d)",
                 DRV8833_M2_IN3, DRV8833_M2_IN4, LEDC_CH_M2_AIN1, LEDC_CH_M2_AIN2);

        // 鸟叫播放器（通过AI音频 I2S 系统播放，无需额外硬件）
        bell_player_ = new BellSoundPlayer();
        bell_player_->Init();
        ESP_LOGI(TAG, "Bell sound player ready");

        // MP3 软解播放器（从 assets 分区读取并解码）
        mp3_ = new Mp3Player();
        mp3_->Init(&Assets::GetInstance());
        ESP_LOGI(TAG, "MP3 soft decoder ready");

        // 光敏传感器 (未接线，暂不初始化)
        // ldr_ = new LdrSensor(LDR_GPIO);

        ESP_LOGI(TAG, "Peripherals initialized");
    }

    void InitializeStateMachine() {
        state_machine_ = new CuckooStateMachine(
            m1_, m2_, m3_, m4_,
            bird_door_motor_, violin_servo_, dog_servo_,
            bird_jump_, mp3_,
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
            app.ToggleChatState();
        });

        // 长按 BOOT 键 1 秒 = 紧急停止（停止所有电机、音乐、AI输出）
        boot_button_.OnLongPress([this]() {
            if (state_machine_) {
                ESP_LOGI(TAG, "BOOT long press: emergency stop");
                state_machine_->StopAll();
            }
        });

        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
        });
    }

public:
    CuckooBoard() : 
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO) {
        
        ESP_LOGI(TAG, "=== Cuckoo Clock Board ===");
        
        InitializeDisplayI2c();
        InitializeDisplay();
        InitializePeripherals();
        InitializeStateMachine();
        InitializeMCPTools();
        InitializeButtons();
        StartClockTask();

        ESP_LOGI(TAG, "Cuckoo board ready! Core 0: AI voice, Core 1: Clock control");
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            display_i2c_bus_,
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
