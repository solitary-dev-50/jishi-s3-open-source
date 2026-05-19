#include "jishi_pan_calibration_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

MossPanCalibrationController::MossPanCalibrationController(MossServoController& servo_controller)
    : servo_controller_(servo_controller) {}

bool MossPanCalibrationController::IsActive() const {
    return active_;
}

void MossPanCalibrationController::SetResult(std::string* result, const std::string& text) const {
    if (result != nullptr) {
        *result = text;
    }
}

std::string MossPanCalibrationController::DescribeStatus() const {
    char buffer[192];
    if (!active_) {
        std::snprintf(buffer, sizeof(buffer), "当前不在水平校准模式");
        return std::string(buffer);
    }
    if (left_saved_ && right_saved_) {
        const float center = (left_pan_ + right_pan_) * 0.5f;
        std::snprintf(buffer,
                      sizeof(buffer),
                      "校准中：当前%.1f，左极限%.1f，右极限%.1f，中点%.1f",
                      current_pan_,
                      left_pan_,
                      right_pan_,
                      center);
        return std::string(buffer);
    }
    std::snprintf(buffer,
                  sizeof(buffer),
                  "校准中：当前%.1f，左极限%s，右极限%s",
                  current_pan_,
                  left_saved_ ? std::to_string(left_pan_).c_str() : "未保存",
                  right_saved_ ? std::to_string(right_pan_).c_str() : "未保存");
    return std::string(buffer);
}

void MossPanCalibrationController::ApplyPose(uint32_t duration_ms,
                                             MossViewPoseController& view_pose_controller,
                                             const std::function<void()>& clear_wake_state,
                                             float& last_applied_pan,
                                             bool& idle_motion_suppressed) {
    current_pan_ = std::clamp(current_pan_,
                              static_cast<float>(SERVO_PAN_MIN_ANGLE),
                              static_cast<float>(SERVO_PAN_MAX_ANGLE));
    view_pose_controller.SetPoseSilently(CameraViewPose::Default);
    idle_motion_suppressed = true;
    clear_wake_state();
    last_applied_pan = current_pan_;
    servo_controller_.LockConversationPan(current_pan_);
    servo_controller_.SetManualPose(SERVO_LISTEN_TILT_ANGLE, current_pan_, duration_ms);
}

bool MossPanCalibrationController::ExecuteAction(ServoPanCalibrationAction action,
                                                 DeviceState current_state,
                                                 MossViewPoseController& view_pose_controller,
                                                 const std::function<void()>& clear_wake_state,
                                                 float& last_applied_pan,
                                                 bool& idle_motion_suppressed,
                                                 std::string* result) {
    switch (action) {
        case ServoPanCalibrationAction::Start:
            active_ = true;
            left_saved_ = false;
            right_saved_ = false;
            servo_controller_.SetPanCalibrationBypass(true);
            current_pan_ = servo_controller_.GetPanCalibrationPhysicalCenterAngle();
            ApplyPose(500,
                      view_pose_controller,
                      clear_wake_state,
                      last_applied_pan,
                      idle_motion_suppressed);
            SetResult(result, "已进入水平校准。说 左调一点 / 右调一点 / 保存左极限 / 保存右极限 / 计算中点 / 退出校准");
            return true;

        case ServoPanCalibrationAction::StepLeft:
            if (!active_) {
                SetResult(result, "请先说 开始水平校准");
                return false;
            }
            current_pan_ += kPanCalibrationStepDeg;
            ApplyPose(180,
                      view_pose_controller,
                      clear_wake_state,
                      last_applied_pan,
                      idle_motion_suppressed);
            SetResult(result, "已左调，当前位置 " + std::to_string(current_pan_));
            return true;

        case ServoPanCalibrationAction::StepRight:
            if (!active_) {
                SetResult(result, "请先说 开始水平校准");
                return false;
            }
            current_pan_ -= kPanCalibrationStepDeg;
            ApplyPose(180,
                      view_pose_controller,
                      clear_wake_state,
                      last_applied_pan,
                      idle_motion_suppressed);
            SetResult(result, "已右调，当前位置 " + std::to_string(current_pan_));
            return true;

        case ServoPanCalibrationAction::SaveLeftLimit:
            if (!active_) {
                SetResult(result, "请先说 开始水平校准");
                return false;
            }
            left_saved_ = true;
            left_pan_ = current_pan_;
            SetResult(result, "已保存左极限 " + std::to_string(left_pan_));
            return true;

        case ServoPanCalibrationAction::SaveRightLimit:
            if (!active_) {
                SetResult(result, "请先说 开始水平校准");
                return false;
            }
            right_saved_ = true;
            right_pan_ = current_pan_;
            SetResult(result, "已保存右极限 " + std::to_string(right_pan_));
            return true;

        case ServoPanCalibrationAction::ComputeCenter:
            if (!active_) {
                SetResult(result, "请先说 开始水平校准");
                return false;
            }
            if (!left_saved_ || !right_saved_) {
                SetResult(result, "请先保存左极限和右极限");
                return false;
            }
            current_pan_ = (left_pan_ + right_pan_) * 0.5f;
            ApplyPose(400,
                      view_pose_controller,
                      clear_wake_state,
                      last_applied_pan,
                      idle_motion_suppressed);
            if (!servo_controller_.SavePanCalibrationProfile(current_pan_,
                                                             std::min(left_pan_, right_pan_),
                                                             std::max(left_pan_, right_pan_))) {
                SetResult(result, "已计算中点，但保存校准失败");
                return false;
            }
            SetResult(result, "已计算并保存中点 " + std::to_string(current_pan_));
            return true;

        case ServoPanCalibrationAction::Status:
            SetResult(result, DescribeStatus());
            return active_;

        case ServoPanCalibrationAction::Exit:
            if (!active_) {
                SetResult(result, "当前不在水平校准模式");
                return false;
            }
            active_ = false;
            if (left_saved_ && right_saved_) {
                servo_controller_.SavePanCalibrationProfile((left_pan_ + right_pan_) * 0.5f,
                                                            std::min(left_pan_, right_pan_),
                                                            std::max(left_pan_, right_pan_));
            }
            servo_controller_.SetPanCalibrationBypass(false);
            servo_controller_.ClearConversationPanLock();
            view_pose_controller.SetPoseSilently(CameraViewPose::Default);
            idle_motion_suppressed = (current_state != kDeviceStateIdle);
            servo_controller_.OnDeviceStateChanged(current_state);
            SetResult(result, "已退出水平校准");
            return true;

        case ServoPanCalibrationAction::None:
        default:
            SetResult(result, "未知校准动作");
            return false;
    }
}

bool MossPanCalibrationController::Adjust(float delta_deg,
                                          MossViewPoseController& view_pose_controller,
                                          const std::function<void()>& clear_wake_state,
                                          float& last_applied_pan,
                                          bool& idle_motion_suppressed,
                                          std::string* result) {
    if (!active_) {
        SetResult(result, "请先说 开始水平校准");
        return false;
    }

    const float previous_pan = current_pan_;
    current_pan_ += delta_deg;
    ApplyPose(180,
              view_pose_controller,
              clear_wake_state,
              last_applied_pan,
              idle_motion_suppressed);

    if (result != nullptr) {
        char buffer[96];
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s%.1f度，当前位置 %.1f",
                      delta_deg >= 0.0f ? "已左调 " : "已右调 ",
                      static_cast<double>(std::fabs(delta_deg)),
                      static_cast<double>(current_pan_));
        *result = std::string(buffer);
        if (std::fabs(current_pan_ - previous_pan) < 0.1f) {
            *result += "，已到边界";
        }
    }
    return true;
}

