#ifndef _JISHI_VIEW_POSE_CONTROLLER_H_
#define _JISHI_VIEW_POSE_CONTROLLER_H_

#include "board.h"
#include "config.h"
#include "device_state.h"
#include "jishi_servo_controller.h"

#include <cstdint>
#include <functional>

class MossViewPoseController {
public:
    explicit MossViewPoseController(MossServoController& servo_controller);

    CameraViewPose GetPose() const;
    bool IsActive() const;
    void SetPoseSilently(CameraViewPose pose);

    float GetTiltForPose(CameraViewPose pose) const;
    float GetCurrentTilt() const;
    void ApplyCurrentPose(uint32_t duration_ms) const;

    bool SetCameraViewPose(CameraViewPose pose,
                           DeviceState current_state,
                           const std::function<void()>& clear_wake_state,
                           float& last_applied_pan,
                           bool& idle_motion_suppressed);

private:
    float ResolveViewPosePan(CameraViewPose pose) const;
    float ResolveDefaultTiltForState(DeviceState state) const;

    MossServoController& servo_controller_;
    CameraViewPose current_pose_ = CameraViewPose::Default;
};

#endif // _JISHI_VIEW_POSE_CONTROLLER_H_

