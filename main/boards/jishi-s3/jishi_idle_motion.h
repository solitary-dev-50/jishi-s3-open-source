#pragma once

#include <cstdint>

class MossIdleMotion {
public:
    struct Pose {
        float tilt = 0.0f;
        float pan = 0.0f;
        uint32_t duration_ms = 0;
    };

    void Initialize() {}
    template <typename T>
    void SetMotionLevel(T) {}
    bool Sample(uint32_t, Pose&) { return false; }
};
