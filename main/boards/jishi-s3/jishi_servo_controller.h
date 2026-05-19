#ifndef _JISHI_SERVO_CONTROLLER_H_
#define _JISHI_SERVO_CONTROLLER_H_

#include "config.h"
#include "device_state.h"

#include <driver/ledc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

class MossServoController {
public:
    struct PanCalibrationProfile {
        bool valid = false;
        float center_angle = static_cast<float>(SERVO_PAN_CENTER_ANGLE);
        float min_angle = static_cast<float>(SERVO_PAN_MIN_ANGLE);
        float max_angle = static_cast<float>(SERVO_PAN_MAX_ANGLE);
    };

    MossServoController();
    ~MossServoController();

    bool Initialize();
    void OnDeviceStateChanged(DeviceState state);
    void Center(uint32_t duration_ms = SERVO_MOVE_DURATION_MS);
    void ClearConversationPanLock();
    void LockConversationPan(float pan_angle);
    float GetConversationPanAngle() const;
    float GetCurrentTiltAngle() const;
    float GetCurrentPanAngle() const;
    void SetMaxSpeeds(float tilt_speed_deg_per_sec, float pan_speed_deg_per_sec);
    void SetManualPose(float tilt, float pan, uint32_t duration_ms);
    void SetIdleMicroMotion(float tilt, float pan, uint32_t duration_ms);
    void SetPanCalibrationBypass(bool enabled);
    bool SavePanCalibrationProfile(float center_angle, float min_angle, float max_angle);
    bool HasPanCalibrationProfile() const;
    float GetPanCalibrationPhysicalCenterAngle() const;

private:
    struct AxisConfig {
        gpio_num_t gpio;
        ledc_channel_t channel;
        float min_angle;
        float max_angle;
        float center_angle;
        float max_speed_deg_per_sec;
        uint32_t min_pulse_us;
        uint32_t center_pulse_us;
        uint32_t max_pulse_us;
    };

    struct MotionState {
        float current_angle;
        float output_angle;
        float start_angle;
        float target_angle;
        uint32_t move_start_ms;
        uint32_t move_duration_ms;
    };

    struct TransitionSequence {
        bool active;
        float final_tilt;
        float final_pan;
        uint32_t final_duration_ms;
    };

    struct BehaviorNoiseState {
        float speaking_tilt_amplitude;
        float listening_pan_phase_offset;
        float speaking_tilt_phase_offset;
        uint32_t suppressed_until_ms;
    };

    static void OnTick(void* arg);
    void UpdateMotion();
    void SetTarget(float tilt, float pan, uint32_t duration_ms);
    void SetTargetInternal(float tilt, float pan, uint32_t duration_ms, bool log);
    void StartTwoStageTransition(float cue_tilt, float cue_pan, uint32_t cue_duration_ms,
                                 float final_tilt, float final_pan, uint32_t final_duration_ms);
    bool LoadPanCalibrationProfile();
    float ComputeListeningPanNoise(uint32_t now_ms) const;
    float ComputeSpeakingTiltNoise(uint32_t now_ms) const;
    float RandomRangeF(float min_value, float max_value) const;
    float ClampAngle(float angle, const AxisConfig& axis) const;
    float ClampPanLogicalAngle(float angle) const;
    float ConvertPanLogicalToPhysical(float angle) const;
    float ApplySpeedLimit(float desired_angle, float current_output_angle,
                          const AxisConfig& axis, float delta_time_sec) const;
    uint32_t AngleToDuty(float angle, const AxisConfig& axis) const;
    float EaseInOutCubic(float t) const;

    bool initialized_ = false;
    bool conversation_pan_locked_ = false;
    float conversation_pan_angle_ = SERVO_CENTER_ANGLE;
    DeviceState current_state_ = kDeviceStateUnknown;
    DeviceState last_state_ = kDeviceStateUnknown;
    uint32_t last_motion_tick_ms_ = 0;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    esp_timer_handle_t tick_timer_ = nullptr;
    AxisConfig tilt_axis_ = {
        SERVO_TILT_GPIO,
        SERVO_TILT_CHANNEL,
        static_cast<float>(SERVO_TILT_MIN_ANGLE),
        static_cast<float>(SERVO_TILT_MAX_ANGLE),
        static_cast<float>(SERVO_TILT_CENTER_ANGLE),
        SERVO_TILT_MAX_SPEED_DEG_PER_SEC,
        SERVO_TILT_MIN_PULSE_US,
        SERVO_TILT_CENTER_PULSE_US,
        SERVO_TILT_MAX_PULSE_US,
    };
    AxisConfig pan_axis_ = {
        SERVO_PAN_GPIO,
        SERVO_PAN_CHANNEL,
        static_cast<float>(SERVO_PAN_MIN_ANGLE),
        static_cast<float>(SERVO_PAN_MAX_ANGLE),
        static_cast<float>(SERVO_PAN_CENTER_ANGLE),
        SERVO_PAN_MAX_SPEED_DEG_PER_SEC,
        SERVO_PAN_MIN_PULSE_US,
        SERVO_PAN_CENTER_PULSE_US,
        SERVO_PAN_MAX_PULSE_US,
    };
    MotionState tilt_state_ = {};
    MotionState pan_state_ = {};
    TransitionSequence transition_sequence_ = {};
    BehaviorNoiseState behavior_noise_ = {};
    bool pan_calibration_bypass_ = false;
    PanCalibrationProfile pan_calibration_ = {};
};

#endif // _JISHI_SERVO_CONTROLLER_H_

