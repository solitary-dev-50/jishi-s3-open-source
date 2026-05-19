#ifndef _JISHI_CAMERA_SESSION_CONTROLLER_H_
#define _JISHI_CAMERA_SESSION_CONTROLLER_H_

#include "esp32_camera.h"
#include "jishi_camera_capture_controller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

class MossCameraSessionController {
public:
    struct Callbacks {
        std::function<bool()> is_document_capture_profile;
        std::function<void()> align_for_document_capture;
        std::function<void(bool)> set_display_updates_suspended;
        std::function<void()> force_camera_off_for_shared_i2c_boot;
        std::function<void()> initialize_camera;
        std::function<void()> power_down_camera_hardware;
        std::function<void()> recover_display_after_camera_use;
        std::function<void()> reset_capture_profile;
        std::function<bool(uint8_t**, size_t*, MossCameraCaptureController::CaptureDebugInfo*)> capture_latest_frame;
        std::function<size_t()> get_internal_sram_free;
        std::function<size_t()> get_internal_sram_largest_block;
        std::function<size_t()> get_internal_sram_minimum;
        std::function<size_t()> get_psram_free;
    };

    explicit MossCameraSessionController(std::atomic<bool>& camera_i2c_occupied);

    bool BeginCaptureSession(bool& lvgl_stopped, const Callbacks& callbacks) const;
    void EndCaptureSession(bool lvgl_stopped, Esp32Camera*& camera, const Callbacks& callbacks) const;
    bool CaptureOnce(Esp32Camera*& camera,
                     uint8_t** out_buf,
                     size_t* out_len,
                     MossCameraCaptureController& capture_controller,
                     const Callbacks& callbacks) const;

private:
    std::atomic<bool>& camera_i2c_occupied_;
};

#endif // _JISHI_CAMERA_SESSION_CONTROLLER_H_

