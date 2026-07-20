#ifndef _WS2812_LED_H_
#define _WS2812_LED_H_

#include "led.h"
#include <driver/gpio.h>
#include <led_strip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

class Ws2812Led : public Led {
public:
    Ws2812Led(gpio_num_t gpio);
    virtual ~Ws2812Led();

    void OnStateChanged() override;

private:
    led_strip_handle_t led_strip_ = nullptr;
    TimerHandle_t blink_timer_ = nullptr;
    bool blink_on_ = false;
    uint8_t r_ = 0, g_ = 0, b_ = 0;  // current base color

    static void BlinkTimerCallback(TimerHandle_t timer);
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
    void TurnOff();
    void StartBlink(uint16_t period_ms);
    void StopBlink();
};

#endif // _WS2812_LED_H_
