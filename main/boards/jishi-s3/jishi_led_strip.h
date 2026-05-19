#ifndef _JISHI_LED_STRIP_H_
#define _JISHI_LED_STRIP_H_

#include "led/led.h"
#include "device_state.h"

#include <driver/gpio.h>
#include <esp_timer.h>
#include <led_strip.h>

#include <cstdint>
#include <mutex>

struct MossLedColor {
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
};

class MossLedStrip : public Led {
public:
    MossLedStrip(gpio_num_t gpio, uint16_t total_leds);
    ~MossLedStrip() override;

    void OnStateChanged() override;

private:
    enum class AnimationMode {
        kStatic,
        kBreathe,
        kBlink,
        kPulse,
    };

    std::mutex mutex_;
    led_strip_handle_t led_strip_ = nullptr;
    esp_timer_handle_t strip_timer_ = nullptr;
    int total_leds_ = 0;
    DeviceState current_state_ = kDeviceStateUnknown;
    AnimationMode mode_ = AnimationMode::kStatic;

    MossLedColor eye_color_{};
    MossLedColor body_color_{};
    MossLedColor body_low_{};
    MossLedColor body_high_{};
    MossLedColor body_base_{};
    MossLedColor blink_eye_on_{};
    MossLedColor blink_body_on_{};

    bool blink_on_ = false;
    bool breathe_increasing_ = true;
    bool pulse_from_input_ = true;
    uint8_t breathe_progress_ = 0;
    uint8_t smooth_level_ = 0;
    uint32_t last_pulse_log_ms_ = 0;

    void StartAnimation(AnimationMode mode, int interval_ms);
    void ApplyFrame(const MossLedColor& eyes, const MossLedColor& body);
    void ClearAll();
    void SetStatic(const MossLedColor& eyes, const MossLedColor& body);
    void SetBreathe(const MossLedColor& eyes, const MossLedColor& body_low, const MossLedColor& body_high, int interval_ms);
    void SetBlink(const MossLedColor& eyes, const MossLedColor& body, int interval_ms);
    void SetPulse(const MossLedColor& eyes, const MossLedColor& body_base, bool input_level, int interval_ms);
    void Tick();
};

#endif

