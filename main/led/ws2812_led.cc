#include "ws2812_led.h"
#include "application.h"

#define TAG "Ws2812Led"

Ws2812Led::Ws2812Led(gpio_num_t gpio) {
    if (gpio == GPIO_NUM_NC) return;

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = 1;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000; // 10MHz

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    blink_timer_ = xTimerCreate("led_blink", pdMS_TO_TICKS(500),
                                pdTRUE, this, BlinkTimerCallback);
}

Ws2812Led::~Ws2812Led() {
    if (blink_timer_) {
        xTimerStop(blink_timer_, 0);
        xTimerDelete(blink_timer_, 0);
    }
    if (led_strip_ != nullptr) {
        led_strip_del(led_strip_);
    }
}

void Ws2812Led::BlinkTimerCallback(TimerHandle_t timer) {
    auto* self = static_cast<Ws2812Led*>(pvTimerGetTimerID(timer));
    if (self->blink_on_) {
        self->TurnOff();
    } else {
        self->SetColor(self->r_, self->g_, self->b_);
    }
    self->blink_on_ = !self->blink_on_;
}

void Ws2812Led::SetColor(uint8_t r, uint8_t g, uint8_t b) {
    if (led_strip_ == nullptr) return;
    led_strip_set_pixel(led_strip_, 0, r, g, b);
    led_strip_refresh(led_strip_);
}

void Ws2812Led::TurnOff() {
    SetColor(0, 0, 0);
}

void Ws2812Led::StartBlink(uint16_t period_ms) {
    if (!blink_timer_) return;
    blink_on_ = true;
    xTimerChangePeriod(blink_timer_, pdMS_TO_TICKS(period_ms), 0);
    xTimerStart(blink_timer_, 0);
}

void Ws2812Led::StopBlink() {
    if (!blink_timer_) return;
    xTimerStop(blink_timer_, 0);
    blink_on_ = false;
}

void Ws2812Led::OnStateChanged() {
    auto& app = Application::GetInstance();
    auto device_state = app.GetDeviceState();

    StopBlink();

    switch (device_state) {
        case kDeviceStateIdle:
            TurnOff();  // 休眠 ⚫
            break;

        case kDeviceStateStarting:
            SetColor(8, 8, 8);  // 启动中 ⚪
            break;

        case kDeviceStateWifiConfiguring:
            r_ = 32; g_ = 32; b_ = 0;
            StartBlink(500);  // WiFi 配网中 🟡 闪烁
            break;

        case kDeviceStateActivating:
            SetColor(32, 0, 32);  // 激活中 🟣
            break;

        case kDeviceStateConnecting:
            SetColor(0, 0, 32);  // 连接 AI ⏚
            break;

        case kDeviceStateListening:
            SetColor(0, 32, 0);  // 聆听 🟢
            break;

        case kDeviceStateSpeaking:
            SetColor(32, 0, 0);  // 说话 🔴
            break;

        case kDeviceStateUpgrading:
            r_ = 32; g_ = 0; b_ = 32;
            StartBlink(1000);  // 升级中 🟣 慢闪
            break;

        case kDeviceStateFatalError:
            SetColor(32, 16, 0);  // 致命错误 🟠
            break;

        default:
            TurnOff();
            break;
    }
}
