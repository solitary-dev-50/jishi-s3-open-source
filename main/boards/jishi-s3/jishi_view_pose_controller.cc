#include "jishi_view_pose_controller.h"

MossViewPoseController::MossViewPoseController(MossServoController& servo_controller)
    : servo_controller_(servo_controller) {}

CameraViewPose MossViewPoseController::GetPose() const {
    return current_pose_;
}

bool MossViewPoseController::IsActive() const {
    return current_pose_ != CameraViewPose::Default;
}

void MossViewPoseController::SetPoseSilently(CameraViewPose pose) {
    current_pose_ = pose;
}

float MossViewPoseController::GetTiltForPose(CameraViewPose pose) const {
    switch (pose) {
        case CameraViewPose::DocumentDown:
            return CAMERA_DOCUMENT_CAPTURE_TILT_ANGLE;
        case CameraViewPose::Up:
            return CAMERA_LOOK_UP_TILT_ANGLE;
        case CameraViewPose::Left:
        case CameraViewPose::Right:
        case CameraViewPose::Default:
        default:
            return SERVO_LISTEN_TILT_ANGLE;
    }
}

float MossViewPoseController::GetCurrentTilt() const {
    return GetTiltForPose(current_pose_);
}

float MossViewPoseController::ResolveViewPosePan(CameraViewPose pose) const {
    switch (pose) {
        case CameraViewPose::Left:
            return CAMERA_LOOK_LEFT_PAN_ANGLE;
        case CameraViewPose::Right:
            return CAMERA_LOOK_RIGHT_PAN_ANGLE;
        case CameraViewPose::DocumentDown:
        case CameraViewPose::Up:
        case CameraViewPose::Default:
        default:
            return static_cast<float>(SERVO_PAN_CENTER_ANGLE);
    }
}

float MossViewPoseController::ResolveDefaultTiltForState(DeviceState state) const {
    switch (state) {
        case kDeviceStateIdle:
        case kDeviceStateActivating:
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
            return SERVO_IDLE_TILT_ANGLE;
        case kDeviceStateConnecting:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        default:
            return SERVO_LISTEN_TILT_ANGLE;
    }
}

void MossViewPoseController::ApplyCurrentPose(uint32_t duration_ms) const {
    if (current_pose_ == CameraViewPose::Default) {
        return;
    }
    servo_controller_.SetManualPose(GetTiltForPose(current_pose_), ResolveViewPosePan(current_pose_), duration_ms);
}

bool MossViewPoseController::SetCameraViewPose(CameraViewPose pose,
                                               DeviceState current_state,
                                               const std::function<void()>& clear_wake_state,
                                               float& last_applied_pan,
                                               bool& idle_motion_suppressed) {
    current_pose_ = pose;
    idle_motion_suppressed = (pose != CameraViewPose::Default);
    clear_wake_state();

    if (pose == CameraViewPose::Default) {
        servo_controller_.ClearConversationPanLock();
        last_applied_pan = static_cast<float>(SERVO_PAN_CENTER_ANGLE);
        servo_controller_.SetManualPose(ResolveDefaultTiltForState(current_state),
                                        static_cast<float>(SERVO_PAN_CENTER_ANGLE),
                                        CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS);
        return true;
    }

    const float pan = ResolveViewPosePan(pose);
    last_applied_pan = pan;
    servo_controller_.LockConversationPan(pan);
    servo_controller_.SetManualPose(GetTiltForPose(pose), pan, CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS);
    return true;
}
