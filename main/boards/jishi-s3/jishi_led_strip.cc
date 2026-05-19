#include "jishi_led_strip.h"

#include "application.h"
#include "config.h"

#include <algorithm>
#include <cassert>

#include <esp_log.h>

namespace {

constexpr const char* TAG = "MossLedStrip";
constexpr uint32_t kPulseLogIntervalMs = 1200;

MossLedColor ScaleColor(const MossLedColor& color, uint8_t scale) {
    return MossLedColor{
        static_cast<uint8_t>((static_cast<uint16_t>(color.red) * scale) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(color.green) * scale) / 255),
        static_cast<uint8_t>((static_cast<uint16_t>(color.blue) * scale) / 255),
    };
}

MossLedColor MixColor(const MossLedColor& low, const MossLedColor& high, uint8_t progress) {
    auto mix = [progress](uint8_t a, uint8_t b) -> uint8_t {
        const int delta = static_cast<int>(b) - static_cast<int>(a);
        return static_cast<uint8_t>(static_cast<int>(a) + ((delta * progress) / 255));
    };
    return MossLedColor{
        mix(low.red, high.red),
        mix(low.green, high.green),
        mix(low.blue, high.blue),
    };
}

const char* StateName(DeviceState state) {
    switch (state) {
        case kDeviceStateStarting: return "starting";
        case kDeviceStateWifiConfiguring: return "wifi_configuring";
        case kDeviceStateIdle: return "idle";
        case kDeviceStateConnecting: return "connecting";
        case kDeviceStateListening: return "listening";
        case kDeviceStateSpeaking: return "speaking";
        case kDeviceStateUpgrading: return "upgrading";
        case kDeviceStateActivating: return "activating";
        case kDeviceStateAudioTesting: return "audio_testing";
        case kDeviceStateFatalError: return "fatal_error";
        default: return "unknown";
    }
}

}  // namespace

MossLedStrip::MossLedStrip(gpio_num_t gpio, uint16_t total_leds) : total_leds_(total_leds) {
    assert(gpio != GPIO_NUM_NC);

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = gpio;
    strip_config.max_leds = total_leds_;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.led_model = LED_MODEL_WS2812;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip_));
    led_strip_clear(led_strip_);

    esp_timer_create_args_t strip_timer_args = {
        .callback = [](void* arg) {
            auto* strip = static_cast<MossLedStrip*>(arg);
            std::lock_guard<std::mutex> lock(strip->mutex_);
            strip->Tick();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "jishi_led_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&strip_timer_args, &strip_timer_));
}

MossLedStrip::~MossLedStrip() {
    if (strip_timer_ != nullptr) {
        esp_timer_stop(strip_timer_);
        esp_timer_delete(strip_timer_);
    }
    if (led_strip_ != nullptr) {
        led_strip_clear(led_strip_);
        led_strip_del(led_strip_);
    }
}

void MossLedStrip::ApplyFrame(const MossLedColor& eyes, const MossLedColor& body) {
    for (int i = 0; i < total_leds_; ++i) {
        const bool is_eye = (i >= LED_EYE_START) && (i < LED_EYE_START + LED_EYE_COUNT);
        const MossLedColor& color = is_eye ? eyes : body;
        led_strip_set_pixel(led_strip_, i, color.red, color.green, color.blue);
    }
    led_strip_refresh(led_strip_);
}

void MossLedStrip::ClearAll() {
    led_strip_clear(led_strip_);
}

void MossLedStrip::StartAnimation(AnimationMode mode, int interval_ms) {
    mode_ = mode;
    esp_timer_stop(strip_timer_);
    if (mode == AnimationMode::kStatic) {
        ApplyFrame(eye_color_, body_color_);
        return;
    }
    esp_timer_start_periodic(strip_timer_, static_cast<uint64_t>(interval_ms) * 1000ULL);
}

void MossLedStrip::SetStatic(const MossLedColor& eyes, const MossLedColor& body) {
    eye_color_ = eyes;
    body_color_ = body;
    StartAnimation(AnimationMode::kStatic, 0);
}

void MossLedStrip::SetBreathe(const MossLedColor& eyes, const MossLedColor& body_low, const MossLedColor& body_high, int interval_ms) {
    eye_color_ = eyes;
    body_low_ = body_low;
    body_high_ = body_high;
    breathe_progress_ = 0;
    breathe_increasing_ = true;
    StartAnimation(AnimationMode::kBreathe, interval_ms);
}

void MossLedStrip::SetBlink(const MossLedColor& eyes, const MossLedColor& body, int interval_ms) {
    blink_eye_on_ = eyes;
    blink_body_on_ = body;
    blink_on_ = false;
    StartAnimation(AnimationMode::kBlink, interval_ms);
}

void MossLedStrip::SetPulse(const MossLedColor& eyes, const MossLedColor& body_base, bool input_level, int interval_ms) {
    eye_color_ = eyes;
    body_base_ = body_base;
    pulse_from_input_ = input_level;
    smooth_level_ = 0;
    last_pulse_log_ms_ = 0;
    StartAnimation(AnimationMode::kPulse, interval_ms);
}

void MossLedStrip::Tick() {
    switch (mode_) {
        case AnimationMode::kBreathe: {
            constexpr uint8_t kStep = 8;
            if (breathe_increasing_) {
                const uint16_t next = static_cast<uint16_t>(breathe_progress_) + kStep;
                if (next >= 255) {
                    breathe_progress_ = 255;
                    breathe_increasing_ = false;
                } else {
                    breathe_progress_ = static_cast<uint8_t>(next);
                }
            } else {
                if (breathe_progress_ <= kStep) {
                    breathe_progress_ = 0;
                    breathe_increasing_ = true;
                } else {
                    breathe_progress_ = static_cast<uint8_t>(breathe_progress_ - kStep);
                }
            }
            body_color_ = MixColor(body_low_, body_high_, breathe_progress_);
            ApplyFrame(eye_color_, body_color_);
            break;
        }
        case AnimationMode::kBlink:
            if (blink_on_) {
                ClearAll();
            } else {
                ApplyFrame(blink_eye_on_, blink_body_on_);
            }
            blink_on_ = !blink_on_;
            break;
        case AnimationMode::kPulse: {
            auto& app = Application::GetInstance();
            const uint8_t target_level = pulse_from_input_
                                             ? app.GetAudioService().GetInputLevel()
                                             : app.GetAudioService().GetOutputLevel();
            smooth_level_ = static_cast<uint8_t>((static_cast<uint16_t>(smooth_level_) * 3 + target_level) / 4);
            const uint8_t scale = static_cast<uint8_t>(72 + (static_cast<uint16_t>(smooth_level_) * 168) / 255);
            body_color_ = ScaleColor(body_base_, scale);
            ApplyFrame(eye_color_, body_color_);

            const uint32_t now_ms = static_cast<uint32_t>(esp_log_timestamp());
            if (now_ms - last_pulse_log_ms_ >= kPulseLogIntervalMs) {
                last_pulse_log_ms_ = now_ms;
                ESP_LOGI(TAG,
                         "Audio pulse [%s]: target=%u smooth=%u scale=%u body_rgb=(%u,%u,%u)",
                         pulse_from_input_ ? "listening" : "speaking",
                         static_cast<unsigned>(target_level),
                         static_cast<unsigned>(smooth_level_),
                         static_cast<unsigned>(scale),
                         static_cast<unsigned>(body_color_.red),
                         static_cast<unsigned>(body_color_.green),
                         static_cast<unsigned>(body_color_.blue));
            }
            break;
        }
        case AnimationMode::kStatic:
        default:
            break;
    }
}

void MossLedStrip::OnStateChanged() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& app = Application::GetInstance();
    const DeviceState state = app.GetDeviceState();
    if (state == current_state_) {
        return;
    }
    current_state_ = state;

    const MossLedColor eye_dim_red{20, 0, 0};
    const MossLedColor eye_warm{26, 8, 0};
    const MossLedColor eye_error{48, 0, 0};
    // Idle blue breathing was visually too strong on the physical body strip.
    // Keep the same hue, but cap the breathing range to about 60% of the
    // previous peak brightness.
    const MossLedColor body_idle_low{0, 1, 11};
    const MossLedColor body_idle_high{0, 17, 72};
    const MossLedColor body_start_low{6, 0, 10};
    const MossLedColor body_start_high{54, 0, 88};
    const MossLedColor body_connecting{0, 26, 72};
    const MossLedColor body_listening{0, 56, 0};
    const MossLedColor body_speaking{72, 28, 0};
    const MossLedColor body_green_alert{0, 42, 6};
    const MossLedColor body_red_alert{72, 0, 0};
    const MossLedColor body_activating{0, 28, 120};

    ESP_LOGI(TAG, "LED mode: %s", StateName(state));

    switch (state) {
        case kDeviceStateStarting:
            SetBreathe(eye_dim_red, body_start_low, body_start_high, 45);
            break;
        case kDeviceStateWifiConfiguring:
        case kDeviceStateIdle:
            SetBreathe(eye_dim_red, body_idle_low, body_idle_high, 45);
            break;
        case kDeviceStateConnecting:
            SetStatic(eye_dim_red, body_connecting);
            break;
        case kDeviceStateListening:
        case kDeviceStateAudioTesting:
            ESP_LOGI(TAG, "LED listening base: eyes=(%u,%u,%u) body=(%u,%u,%u)",
                     eye_dim_red.red, eye_dim_red.green, eye_dim_red.blue,
                     body_listening.red, body_listening.green, body_listening.blue);
            SetPulse(eye_dim_red, body_listening, true, 50);
            break;
        case kDeviceStateSpeaking:
            ESP_LOGI(TAG, "LED speaking base: eyes=(%u,%u,%u) body=(%u,%u,%u)",
                     eye_warm.red, eye_warm.green, eye_warm.blue,
                     body_speaking.red, body_speaking.green, body_speaking.blue);
            SetPulse(eye_warm, body_speaking, false, 50);
            break;
        case kDeviceStateUpgrading:
            SetBlink(eye_dim_red, body_green_alert, 120);
            break;
        case kDeviceStateActivating:
            SetBlink(eye_dim_red, body_activating, 400);
            break;
        case kDeviceStateFatalError:
            SetBlink(eye_error, body_red_alert, 180);
            break;
        case kDeviceStateUnknown:
        default:
            SetStatic({0, 0, 0}, {0, 0, 0});
            break;
    }
}

