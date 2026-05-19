#include "jishi_servo_controller.h"

#include <algorithm>
#include <cmath>

#include <esp_log.h>
#include <driver/ledc.h>

namespace {
constexpr const char* TAG = "MossServo";
#if SERVO_PWM_FREQUENCY_HZ <= 0
#error "SERVO_PWM_FREQUENCY_HZ must be greater than 0"
#endif
constexpr uint32_t kServoPwmFrequencyHz = SERVO_PWM_FREQUENCY_HZ;
constexpr uint32_t kServoPeriodUs = 1000000u / kServoPwmFrequencyHz;
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_bit_t kDutyResolution = LEDC_TIMER_14_BIT;
constexpr uint32_t kDutyMax = (1u << 14) - 1u;
}

MossServoController::MossServoController() {
    tilt_state_.current_angle = tilt_axis_.center_angle;
    tilt_state_.output_angle = tilt_axis_.center_angle;
    tilt_state_.start_angle = tilt_axis_.center_angle;
    tilt_state_.target_angle = tilt_axis_.center_angle;
    tilt_state_.move_duration_ms = SERVO_MOVE_DURATION_MS;

    pan_state_.current_angle = pan_axis_.center_angle;
    pan_state_.output_angle = pan_axis_.center_angle;
    pan_state_.start_angle = pan_axis_.center_angle;
    pan_state_.target_angle = pan_axis_.center_angle;
    pan_state_.move_duration_ms = SERVO_MOVE_DURATION_MS;

    conversation_pan_angle_ = static_cast<float>(SERVO_PAN_CENTER_ANGLE);
}

MossServoController::~MossServoController() {
    if (tick_timer_ != nullptr) {
        esp_timer_stop(tick_timer_);
        esp_timer_delete(tick_timer_);
        tick_timer_ = nullptr;
    }
}

bool MossServoController::Initialize() {
    if (initialized_) {
        return true;
    }

#if !SERVO_OUTPUT_ENABLED
    initialized_ = true;
    ESP_LOGW(TAG, "Servo output disabled by build switch");
    return true;
#endif

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = kSpeedMode;
    timer_config.timer_num = SERVO_LEDC_TIMER;
    timer_config.duty_resolution = kDutyResolution;
    timer_config.freq_hz = kServoPwmFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_config));

    const AxisConfig axes[] = {tilt_axis_, pan_axis_};
    for (const auto& axis : axes) {
        ledc_channel_config_t channel_config = {};
        channel_config.gpio_num = axis.gpio;
        channel_config.speed_mode = kSpeedMode;
        channel_config.channel = axis.channel;
        channel_config.intr_type = LEDC_INTR_DISABLE;
        channel_config.timer_sel = SERVO_LEDC_TIMER;
        channel_config.duty = AngleToDuty(axis.center_angle, axis);
        channel_config.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&channel_config));
    }

    initialized_ = true;
    Center(0);
    ESP_LOGI(TAG, "Open-source basic servo controller initialized");
    return true;
}

void MossServoController::OnDeviceStateChanged(DeviceState state) {
    current_state_ = state;
    last_state_ = state;
    if (!initialized_) {
        return;
    }

    switch (state) {
        case kDeviceStateIdle:
        case kDeviceStateActivating:
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
            Center(0);
            break;
        case kDeviceStateListening:
        case kDeviceStateConnecting:
        case kDeviceStateSpeaking:
        default:
            SetTarget(SERVO_LISTEN_TILT_ANGLE,
                      conversation_pan_locked_ ? conversation_pan_angle_ : static_cast<float>(SERVO_PAN_CENTER_ANGLE),
                      0);
            break;
    }
}

void MossServoController::Center(uint32_t duration_ms) {
    SetTarget(SERVO_IDLE_TILT_ANGLE, SERVO_IDLE_PAN_ANGLE, duration_ms);
}

void MossServoController::ClearConversationPanLock() {
    conversation_pan_locked_ = false;
    conversation_pan_angle_ = static_cast<float>(SERVO_PAN_CENTER_ANGLE);
}

void MossServoController::LockConversationPan(float pan_angle) {
    conversation_pan_locked_ = true;
    conversation_pan_angle_ = ClampPanLogicalAngle(pan_angle);
}

float MossServoController::GetConversationPanAngle() const {
    return conversation_pan_angle_;
}

float MossServoController::GetCurrentTiltAngle() const {
    return tilt_state_.output_angle;
}

float MossServoController::GetCurrentPanAngle() const {
    return pan_state_.output_angle;
}

void MossServoController::SetMaxSpeeds(float tilt_speed_deg_per_sec, float pan_speed_deg_per_sec) {
    tilt_axis_.max_speed_deg_per_sec = std::max(0.0f, tilt_speed_deg_per_sec);
    pan_axis_.max_speed_deg_per_sec = std::max(0.0f, pan_speed_deg_per_sec);
}

void MossServoController::SetManualPose(float tilt, float pan, uint32_t duration_ms) {
    SetTarget(tilt, pan, duration_ms);
}

void MossServoController::SetIdleMicroMotion(float tilt, float pan, uint32_t duration_ms) {
    SetTarget(tilt, pan, duration_ms);
}

void MossServoController::OnTick(void* arg) {
    auto* self = static_cast<MossServoController*>(arg);
    if (self != nullptr) {
        self->UpdateMotion();
    }
}

void MossServoController::UpdateMotion() {}

void MossServoController::SetTarget(float tilt, float pan, uint32_t duration_ms) {
    SetTargetInternal(tilt, pan, duration_ms, true);
}

void MossServoController::SetTargetInternal(float tilt, float pan, uint32_t duration_ms, bool) {
    const float clamped_tilt = ClampAngle(tilt, tilt_axis_);
    const float clamped_pan = ClampPanLogicalAngle(pan);

    tilt_state_.start_angle = tilt_state_.output_angle;
    tilt_state_.target_angle = clamped_tilt;
    tilt_state_.current_angle = clamped_tilt;
    tilt_state_.output_angle = clamped_tilt;
    tilt_state_.move_duration_ms = duration_ms;

    pan_state_.start_angle = pan_state_.output_angle;
    pan_state_.target_angle = clamped_pan;
    pan_state_.current_angle = clamped_pan;
    pan_state_.output_angle = clamped_pan;
    pan_state_.move_duration_ms = duration_ms;

#if SERVO_OUTPUT_ENABLED
    if (initialized_) {
        const uint32_t tilt_duty = AngleToDuty(clamped_tilt, tilt_axis_);
        ledc_set_duty(kSpeedMode, tilt_axis_.channel, tilt_duty);
        ledc_update_duty(kSpeedMode, tilt_axis_.channel);

        const float physical_pan = ConvertPanLogicalToPhysical(clamped_pan);
        const uint32_t pan_duty = AngleToDuty(physical_pan, pan_axis_);
        ledc_set_duty(kSpeedMode, pan_axis_.channel, pan_duty);
        ledc_update_duty(kSpeedMode, pan_axis_.channel);
    }
#endif
}

void MossServoController::StartTwoStageTransition(float cue_tilt,
                                                  float cue_pan,
                                                  uint32_t cue_duration_ms,
                                                  float,
                                                  float,
                                                  uint32_t) {
    SetTarget(cue_tilt, cue_pan, cue_duration_ms);
}

bool MossServoController::LoadPanCalibrationProfile() {
    pan_calibration_.valid = false;
    pan_calibration_.center_angle = static_cast<float>(SERVO_PAN_CENTER_ANGLE);
    pan_calibration_.min_angle = static_cast<float>(SERVO_PAN_MIN_ANGLE);
    pan_calibration_.max_angle = static_cast<float>(SERVO_PAN_MAX_ANGLE);
    return false;
}

float MossServoController::ComputeListeningPanNoise(uint32_t) const { return 0.0f; }
float MossServoController::ComputeSpeakingTiltNoise(uint32_t) const { return 0.0f; }
float MossServoController::RandomRangeF(float min_value, float max_value) const { return (min_value + max_value) * 0.5f; }

float MossServoController::ClampAngle(float angle, const AxisConfig& axis) const {
    return std::clamp(angle, axis.min_angle, axis.max_angle);
}

float MossServoController::ClampPanLogicalAngle(float angle) const {
    const float min_angle = pan_calibration_bypass_ || !pan_calibration_.valid
        ? static_cast<float>(SERVO_PAN_MIN_ANGLE)
        : pan_calibration_.min_angle;
    const float max_angle = pan_calibration_bypass_ || !pan_calibration_.valid
        ? static_cast<float>(SERVO_PAN_MAX_ANGLE)
        : pan_calibration_.max_angle;
    return std::clamp(angle, min_angle, max_angle);
}

float MossServoController::ConvertPanLogicalToPhysical(float angle) const {
    return ClampPanLogicalAngle(angle);
}

float MossServoController::ApplySpeedLimit(float desired_angle, float, const AxisConfig& axis, float) const {
    return ClampAngle(desired_angle, axis);
}

uint32_t MossServoController::AngleToDuty(float angle, const AxisConfig& axis) const {
    const float clamped = ClampAngle(angle, axis);
    const float angle_span = axis.max_angle - axis.min_angle;
    const float pulse_span = static_cast<float>(axis.max_pulse_us - axis.min_pulse_us);
    const float ratio = angle_span > 0.0f ? (clamped - axis.min_angle) / angle_span : 0.5f;
    const float pulse_us = static_cast<float>(axis.min_pulse_us) + ratio * pulse_span;
    const float duty = (pulse_us / static_cast<float>(kServoPeriodUs)) * static_cast<float>(kDutyMax);
    return static_cast<uint32_t>(std::clamp(duty, 0.0f, static_cast<float>(kDutyMax)));
}

float MossServoController::EaseInOutCubic(float t) const {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped < 0.5f
        ? 4.0f * clamped * clamped * clamped
        : 1.0f - std::pow(-2.0f * clamped + 2.0f, 3.0f) / 2.0f;
}

void MossServoController::SetPanCalibrationBypass(bool enabled) {
    pan_calibration_bypass_ = enabled;
}

bool MossServoController::SavePanCalibrationProfile(float center_angle, float min_angle, float max_angle) {
    if (min_angle > max_angle) {
        std::swap(min_angle, max_angle);
    }
    pan_calibration_.valid = true;
    pan_calibration_.center_angle = center_angle;
    pan_calibration_.min_angle = min_angle;
    pan_calibration_.max_angle = max_angle;
    return true;
}

bool MossServoController::HasPanCalibrationProfile() const {
    return pan_calibration_.valid;
}

float MossServoController::GetPanCalibrationPhysicalCenterAngle() const {
    return pan_calibration_.valid ? pan_calibration_.center_angle : static_cast<float>(SERVO_PAN_CENTER_ANGLE);
}
