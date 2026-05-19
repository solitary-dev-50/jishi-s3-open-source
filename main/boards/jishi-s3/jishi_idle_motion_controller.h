#pragma once

#include "device_state.h"
#include "jishi_idle_motion.h"
#include "jishi_servo_controller.h"

class MossIdleMotionController {
public:
    MossIdleMotionController(MossIdleMotion&, MossServoController&) {}
    void Initialize() {}
    void UpdateBatteryPolicy(bool, int, bool) {}
    void Poll(DeviceState, bool) const {}
};

