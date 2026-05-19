#ifndef _JISHI_CAMERA_HARDWARE_CONTROLLER_H_
#define _JISHI_CAMERA_HARDWARE_CONTROLLER_H_

#include "board.h"
#include "esp32_camera.h"
#include "jishi_camera_capture_controller.h"

#include <cstddef>
#include <cstdint>

#include <esp_lcd_panel_ops.h>

class Display;

class MossCameraHardwareController {
public:
    MossCameraHardwareController(Esp32Camera*& camera,
                                 MossCameraCaptureController& capture_controller,
                                 Display*& display,
                                 esp_lcd_panel_handle_t& panel);

    void InitializeCamera();
    void PowerDownCameraHardware();
    bool CaptureLatestFrameToPsram(uint8_t** out_buf,
                                   size_t* out_len,
                                   MossCameraCaptureController::CaptureDebugInfo* debug);
    void SetDisplayUpdatesSuspended(bool suspended) const;
    void RecoverDisplayAfterCameraUse() const;

private:
    void InitializeCameraPowerSequence() const;
    void DiscardCameraFrames(int count, const char* reason) const;
    void WarmupCameraFrames() const;
    void TuneOv5640Image() const;
    bool ShouldRunAutofocusForCurrentProfile() const;
    void InitializeOv5640Autofocus();
    bool WaitForOv5640Autofocus(sensor_t* sensor, uint32_t timeout_ms, const char* stage);
    bool RunOv5640AutofocusAutoFallback(sensor_t* sensor);
    void RunOv5640AutofocusBeforeCapture();
    bool IsDocumentCaptureProfile() const;

    static float ComputeRgb565Sharpness(const uint8_t* jpeg_buf,
                                        size_t jpeg_len,
                                        size_t* out_width,
                                        size_t* out_height);

    Esp32Camera*& camera_;
    MossCameraCaptureController& capture_controller_;
    Display*& display_;
    esp_lcd_panel_handle_t& panel_;
    bool camera_af_ready_ = false;
    bool last_af_attempted_ = false;
    bool last_af_focused_ = false;
    uint8_t last_af_raw_ = 0;
};

#endif // _JISHI_CAMERA_HARDWARE_CONTROLLER_H_

