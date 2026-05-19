#ifndef _JISHI_PAN_CALIBRATION_CONTROLLER_H_
#define _JISHI_PAN_CALIBRATION_CONTROLLER_H_

#include "board.h"
#include "config.h"
#include "device_state.h"
#include "jishi_servo_controller.h"
#include "jishi_view_pose_controller.h"

#include <cstdint>
#include <functional>
#include <string>

class MossPanCalibrationController {
public:
    explicit MossPanCalibrationController(MossServoController& servo_controller);

    bool IsActive() const;
    void ApplyPose(uint32_t duration_ms,
                   MossViewPoseController& view_pose_controller,
                   const std::function<void()>& clear_wake_state,
                   float& last_applied_pan,
                   bool& idle_motion_suppressed);

    bool ExecuteAction(ServoPanCalibrationAction action,
                       DeviceState current_state,
                       MossViewPoseController& view_pose_controller,
                       const std::function<void()>& clear_wake_state,
                       float& last_applied_pan,
                       bool& idle_motion_suppressed,
                       std::string* result = nullptr);

    bool Adjust(float delta_deg,
                MossViewPoseController& view_pose_controller,
                const std::function<void()>& clear_wake_state,
                float& last_applied_pan,
                bool& idle_motion_suppressed,
                std::string* result = nullptr);

private:
    static constexpr float kPanCalibrationStepDeg = 2.0f;

    std::string DescribeStatus() const;
    void SetResult(std::string* result, const std::string& text) const;

    MossServoController& servo_controller_;
    bool active_ = false;
    bool left_saved_ = false;
    bool right_saved_ = false;
    float current_pan_ = SERVO_PAN_CENTER_ANGLE;
    float left_pan_ = SERVO_PAN_CENTER_ANGLE;
    float right_pan_ = SERVO_PAN_CENTER_ANGLE;
};

#endif // _JISHI_PAN_CALIBRATION_CONTROLLER_H_

