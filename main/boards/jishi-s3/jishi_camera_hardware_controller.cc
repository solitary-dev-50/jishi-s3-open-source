#include "jishi_camera_hardware_controller.h"

#include "config.h"
#include "display/display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"

#include <cmath>
#include <cstring>

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_camera.h>
#include <esp_camera_af.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "MossCamHw";
}

MossCameraHardwareController::MossCameraHardwareController(Esp32Camera*& camera,
                                                           MossCameraCaptureController& capture_controller,
                                                           Display*& display,
                                                           esp_lcd_panel_handle_t& panel)
    : camera_(camera),
      capture_controller_(capture_controller),
      display_(display),
      panel_(panel) {}

bool MossCameraHardwareController::IsDocumentCaptureProfile() const {
    return capture_controller_.IsDocumentCaptureProfile();
}

void MossCameraHardwareController::InitializeCameraPowerSequence() const {
    ESP_LOGI(TAG, "Camera cold-start: enable LDO");
    gpio_config_t gpio_cfg = {
        .pin_bit_mask = (1ULL << CAMERA_PIN_LDO_EN) |
                        (1ULL << CAMERA_PIN_PWDN) |
                        (1ULL << CAMERA_PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio_cfg));

    gpio_set_level(CAMERA_PIN_LDO_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_POWER_LDO_STABILIZE_DELAY_MS));

    ESP_LOGI(TAG, "Camera cold-start: exit powerdown");
    gpio_set_level(CAMERA_PIN_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_POWER_EXIT_PWDN_DELAY_MS));

    ESP_LOGI(TAG, "Camera cold-start: hardware reset");
    gpio_set_level(CAMERA_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_POWER_RESET_PULSE_DELAY_MS));
    gpio_set_level(CAMERA_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(CAMERA_POWER_RESET_RELEASE_DELAY_MS));
}

void MossCameraHardwareController::DiscardCameraFrames(int count, const char* reason) const {
    for (int i = 0; i < count; ++i) {
        camera_fb_t* frame = esp_camera_fb_get();
        if (frame == nullptr) {
            ESP_LOGW(TAG, "Camera frame discard failed (%s) at index=%d", reason, i + 1);
            break;
        }
        esp_camera_fb_return(frame);
    }
}

void MossCameraHardwareController::WarmupCameraFrames() const {
    const int warmup_frames = IsDocumentCaptureProfile()
        ? CAMERA_DOCUMENT_WARMUP_FRAME_COUNT
        : CAMERA_WARMUP_FRAME_COUNT;
    DiscardCameraFrames(warmup_frames, "warmup");
    ESP_LOGI(TAG, "Camera warmup complete (%d frames)", warmup_frames);
}

void MossCameraHardwareController::TuneOv5640Image() const {
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "OV5640 tuning skipped: sensor handle is null");
        return;
    }
    if (sensor->id.PID != OV5640_PID) {
        return;
    }

    const bool document_profile = IsDocumentCaptureProfile();
    const int brightness = document_profile ? CAMERA_DOCUMENT_OV5640_BRIGHTNESS : CAMERA_OV5640_BRIGHTNESS;
    const int contrast = document_profile ? CAMERA_DOCUMENT_OV5640_CONTRAST : CAMERA_OV5640_CONTRAST;
    const int sharpness = document_profile ? CAMERA_DOCUMENT_OV5640_SHARPNESS : CAMERA_OV5640_SHARPNESS;
    const int ae_level = document_profile ? CAMERA_DOCUMENT_OV5640_AE_LEVEL : CAMERA_OV5640_AE_LEVEL;
    const gainceiling_t gainceiling = document_profile ? CAMERA_DOCUMENT_OV5640_GAINCEILING : CAMERA_OV5640_GAINCEILING;

    sensor->set_gain_ctrl(sensor, 1);
    sensor->set_exposure_ctrl(sensor, 1);
    sensor->set_aec2(sensor, 1);
    sensor->set_gainceiling(sensor, gainceiling);
    sensor->set_ae_level(sensor, ae_level);
    sensor->set_brightness(sensor, brightness);
    sensor->set_contrast(sensor, contrast);
    sensor->set_sharpness(sensor, sharpness);

    ESP_LOGI(TAG,
             "OV5640 tuned (%s): brightness=%d contrast=%d sharpness=%d ae_level=%d gainceiling=%d",
             document_profile ? "document" : "default",
             brightness,
             contrast,
             sharpness,
             ae_level,
             static_cast<int>(gainceiling));
}

bool MossCameraHardwareController::ShouldRunAutofocusForCurrentProfile() const {
    return IsDocumentCaptureProfile() || CAMERA_DEFAULT_AF_ENABLED;
}

void MossCameraHardwareController::InitializeOv5640Autofocus() {
    camera_af_ready_ = false;

    if (!ShouldRunAutofocusForCurrentProfile()) {
        ESP_LOGI(TAG, "OV5640 AF init skipped for fast default capture");
        return;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr) {
        ESP_LOGW(TAG, "OV5640 AF init skipped: sensor handle is null");
        return;
    }
    if (sensor->id.PID != OV5640_PID) {
        ESP_LOGI(TAG, "OV5640 AF init skipped: PID=%#x", sensor->id.PID);
        return;
    }

    if (!esp_camera_af_is_supported(sensor)) {
        ESP_LOGW(TAG, "OV5640 AF is not enabled or not supported by current build");
        return;
    }

    esp_camera_af_config_t af_cfg = {};
    af_cfg.mode = ESP_CAMERA_AF_MODE_MANUAL;
    af_cfg.step_size = 8;
    af_cfg.range_min = 0;
    af_cfg.range_max = 1023;
    af_cfg.timeout_ms = CAMERA_AF_TIMEOUT_MS;

    const esp_err_t ret = esp_camera_af_init(sensor, &af_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5640 AF init failed: %s", esp_err_to_name(ret));
        return;
    }

    camera_af_ready_ = true;
    ESP_LOGI(TAG, "OV5640 AF init complete: timeout=%dms", CAMERA_AF_TIMEOUT_MS);
}

bool MossCameraHardwareController::WaitForOv5640Autofocus(sensor_t* sensor,
                                                          uint32_t timeout_ms,
                                                          const char* stage) {
    esp_camera_af_status_t status = {};
    const esp_err_t ret = esp_camera_af_wait(sensor, timeout_ms, &status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5640 AF %s wait failed: %s", stage, esp_err_to_name(ret));
        return false;
    }

    esp_camera_af_status_t status_copy{};
    memcpy(&status_copy, &status, sizeof(status_copy));
    last_af_focused_ = status_copy.focused;
    last_af_raw_ = status_copy.raw;
    ESP_LOGI(TAG,
             "OV5640 AF %s result: focused=%d busy=%d raw=0x%02x",
             stage,
             status_copy.focused,
             status_copy.busy,
             status_copy.raw);
    return status_copy.focused;
}

bool MossCameraHardwareController::RunOv5640AutofocusAutoFallback(sensor_t* sensor) {
    const esp_err_t set_auto_ret = esp_camera_af_set_mode(sensor, ESP_CAMERA_AF_MODE_AUTO);
    if (set_auto_ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5640 AF fallback set AUTO failed: %s", esp_err_to_name(set_auto_ret));
        return false;
    }

    ESP_LOGI(TAG, "OV5640 AF fallback: retry with AUTO mode");
    const bool focused = WaitForOv5640Autofocus(sensor,
                                                CAMERA_AF_AUTO_FALLBACK_TIMEOUT_MS,
                                                "fallback");

    const esp_err_t set_manual_ret = esp_camera_af_set_mode(sensor, ESP_CAMERA_AF_MODE_MANUAL);
    if (set_manual_ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5640 AF fallback restore MANUAL failed: %s",
                 esp_err_to_name(set_manual_ret));
    }
    return focused;
}

void MossCameraHardwareController::RunOv5640AutofocusBeforeCapture() {
    last_af_attempted_ = false;
    last_af_focused_ = false;
    last_af_raw_ = 0;
    if (!ShouldRunAutofocusForCurrentProfile()) {
        return;
    }
    if (!camera_af_ready_) {
        return;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor == nullptr || sensor->id.PID != OV5640_PID) {
        return;
    }

    last_af_attempted_ = true;
    bool focused = false;
    const esp_err_t trigger_ret = esp_camera_af_trigger(sensor);
    if (trigger_ret != ESP_OK) {
        ESP_LOGW(TAG, "OV5640 AF trigger failed: %s", esp_err_to_name(trigger_ret));
        focused = RunOv5640AutofocusAutoFallback(sensor);
    } else {
        focused = WaitForOv5640Autofocus(sensor, CAMERA_AF_TIMEOUT_MS, "single");
        if (!focused) {
            focused = RunOv5640AutofocusAutoFallback(sensor);
        }
    }

    if (!focused) {
        ESP_LOGW(TAG, "OV5640 AF final result is still not focused");
    }

    vTaskDelay(pdMS_TO_TICKS(CAMERA_AF_SETTLE_DELAY_MS));
    DiscardCameraFrames(CAMERA_AF_POST_TRIGGER_DISCARD_FRAMES, "af_settle");
}

void MossCameraHardwareController::PowerDownCameraHardware() {
    ESP_LOGI(TAG, "Camera power-down: enter standby");
    camera_af_ready_ = false;
    gpio_set_level(CAMERA_PIN_PWDN, 1);
    gpio_set_level(CAMERA_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "Camera power-down: disable LDO");
    gpio_set_level(CAMERA_PIN_LDO_EN, 0);
}

void MossCameraHardwareController::InitializeCamera() {
    if (camera_ != nullptr) {
        return;
    }

    InitializeCameraPowerSequence();

    camera_config_t config = {};
    config.ledc_channel = CAMERA_XCLK_LEDC_CHANNEL;
    config.ledc_timer = CAMERA_XCLK_LEDC_TIMER;
    config.pin_d0 = CAMERA_PIN_D0;
    config.pin_d1 = CAMERA_PIN_D1;
    config.pin_d2 = CAMERA_PIN_D2;
    config.pin_d3 = CAMERA_PIN_D3;
    config.pin_d4 = CAMERA_PIN_D4;
    config.pin_d5 = CAMERA_PIN_D5;
    config.pin_d6 = CAMERA_PIN_D6;
    config.pin_d7 = CAMERA_PIN_D7;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = -1;
    config.pin_sccb_scl = -1;
    config.sccb_i2c_port = CAMERA_SCCB_I2C_PORT;
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.xclk_freq_hz = CAMERA_XCLK_FREQ_HZ;
    config.pixel_format = CAMERA_PIXEL_FORMAT;
    config.frame_size = IsDocumentCaptureProfile() ? CAMERA_DOCUMENT_FRAME_SIZE : CAMERA_FRAME_SIZE;
    config.jpeg_quality = IsDocumentCaptureProfile() ? CAMERA_DOCUMENT_JPEG_QUALITY : CAMERA_JPEG_QUALITY;
    config.fb_count = CAMERA_FB_COUNT;
    config.fb_location = CAMERA_FB_LOCATION;
    config.grab_mode = CAMERA_GRAB_MODE;

    ESP_LOGI(TAG,
             "Camera capture profile: %s frame_size=%d jpeg_quality=%d",
             IsDocumentCaptureProfile() ? "document" : "default",
             static_cast<int>(config.frame_size),
             static_cast<int>(config.jpeg_quality));

    camera_ = new Esp32Camera(config);
    if (!camera_->IsReady()) {
        delete camera_;
        camera_ = nullptr;
        return;
    }
    camera_->SetHMirror(CAMERA_HMIRROR);
    camera_->SetVFlip(CAMERA_VFLIP);
    TuneOv5640Image();
    WarmupCameraFrames();
    InitializeOv5640Autofocus();
}

float MossCameraHardwareController::ComputeRgb565Sharpness(const uint8_t* jpeg_buf,
                                                           size_t jpeg_len,
                                                           size_t* out_width,
                                                           size_t* out_height) {
    if (out_width != nullptr) {
        *out_width = 0;
    }
    if (out_height != nullptr) {
        *out_height = 0;
    }
    if (jpeg_buf == nullptr || jpeg_len == 0) {
        return -1.0f;
    }

    uint8_t* rgb565 = nullptr;
    size_t rgb565_len = 0;
    size_t width = 0;
    size_t height = 0;
    size_t stride = 0;
    if (jpeg_to_image(jpeg_buf, jpeg_len, &rgb565, &rgb565_len, &width, &height, &stride) != ESP_OK ||
        rgb565 == nullptr || width < 3 || height < 3 || stride < width * 2) {
        if (rgb565 != nullptr) {
            heap_caps_free(rgb565);
        }
        return -1.0f;
    }

    if (out_width != nullptr) {
        *out_width = width;
    }
    if (out_height != nullptr) {
        *out_height = height;
    }

    auto luminance = [](uint16_t pixel) -> int {
        const int r = ((pixel >> 11) & 0x1F) * 255 / 31;
        const int g = ((pixel >> 5) & 0x3F) * 255 / 63;
        const int b = (pixel & 0x1F) * 255 / 31;
        return (r * 30 + g * 59 + b * 11) / 100;
    };

    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(rgb565);
    const size_t stride_pixels = stride / 2;
    double laplacian_abs_sum = 0.0;
    double luminance_sum = 0.0;
    size_t dark_count = 0;
    size_t bright_count = 0;
    size_t sample_count = 0;

    for (size_t y = 1; y + 1 < height; y += 2) {
        for (size_t x = 1; x + 1 < width; x += 2) {
            const int c = luminance(pixels[y * stride_pixels + x]);
            const int l = luminance(pixels[y * stride_pixels + (x - 1)]);
            const int r = luminance(pixels[y * stride_pixels + (x + 1)]);
            const int u = luminance(pixels[(y - 1) * stride_pixels + x]);
            const int d = luminance(pixels[(y + 1) * stride_pixels + x]);
            const int lap = 4 * c - l - r - u - d;
            laplacian_abs_sum += std::abs(lap);
            luminance_sum += c;
            if (c <= 18) {
                ++dark_count;
            } else if (c >= 245) {
                ++bright_count;
            }
            ++sample_count;
        }
    }

    heap_caps_free(rgb565);
    if (sample_count == 0) {
        return -1.0f;
    }
    const float sharpness = static_cast<float>(laplacian_abs_sum / static_cast<double>(sample_count));
    const float mean_luma = static_cast<float>(luminance_sum / static_cast<double>(sample_count));
    const float dark_ratio = static_cast<float>(dark_count) / static_cast<float>(sample_count);
    const float bright_ratio = static_cast<float>(bright_count) / static_cast<float>(sample_count);

    float exposure_penalty = 1.0f;
    if (mean_luma < 28.0f || dark_ratio > 0.65f) {
        exposure_penalty *= 0.08f;
    } else if (mean_luma < 45.0f || dark_ratio > 0.45f) {
        exposure_penalty *= 0.35f;
    }
    if (mean_luma > 228.0f || bright_ratio > 0.55f) {
        exposure_penalty *= 0.12f;
    } else if (mean_luma > 205.0f || bright_ratio > 0.30f) {
        exposure_penalty *= 0.45f;
    }

    return sharpness * exposure_penalty;
}

bool MossCameraHardwareController::CaptureLatestFrameToPsram(uint8_t** out_buf,
                                                             size_t* out_len,
                                                             MossCameraCaptureController::CaptureDebugInfo* debug) {
    RunOv5640AutofocusBeforeCapture();

    const int candidate_count = IsDocumentCaptureProfile() ? CAMERA_DOCUMENT_BURST_COUNT : 1;
    uint8_t* best_buf = nullptr;
    size_t best_len = 0;
    size_t best_width = 0;
    size_t best_height = 0;
    float best_score = -1.0f;
    int best_index = -1;

    for (int i = 0; i < candidate_count; ++i) {
        camera_fb_t* stale_fb = esp_camera_fb_get();
        if (stale_fb == nullptr) {
            ESP_LOGE(TAG, "CaptureOnce stale frame fetch failed");
            break;
        }
        esp_camera_fb_return(stale_fb);

        camera_fb_t* latest_fb = esp_camera_fb_get();
        if (latest_fb == nullptr) {
            ESP_LOGE(TAG, "CaptureOnce latest frame fetch failed");
            break;
        }

        uint8_t* candidate_buf = nullptr;
        size_t candidate_len = 0;
        size_t candidate_width = latest_fb->width;
        size_t candidate_height = latest_fb->height;
        float candidate_score = -1.0f;

        bool ok = false;
        do {
            if (latest_fb->format != PIXFORMAT_JPEG) {
                ESP_LOGE(TAG, "CaptureOnce only supports JPEG output, got format=%d", latest_fb->format);
                break;
            }

            candidate_buf = static_cast<uint8_t*>(
                heap_caps_malloc(latest_fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (candidate_buf == nullptr) {
                ESP_LOGE(TAG, "CaptureOnce PSRAM allocation failed, len=%u", static_cast<unsigned>(latest_fb->len));
                break;
            }

            memcpy(candidate_buf, latest_fb->buf, latest_fb->len);
            candidate_len = latest_fb->len;
            candidate_score = ComputeRgb565Sharpness(candidate_buf,
                                                     candidate_len,
                                                     &candidate_width,
                                                     &candidate_height);
            ok = true;
        } while (false);

        esp_camera_fb_return(latest_fb);

        if (debug != nullptr && i < CAMERA_DOCUMENT_BURST_COUNT) {
            debug->candidate_scores[i] = candidate_score;
        }

        ESP_LOGI(TAG,
                 "Capture candidate %d/%d: ok=%d jpeg_len=%u sharpness=%.2f size=%ux%u",
                 i + 1,
                 candidate_count,
                 ok ? 1 : 0,
                 static_cast<unsigned>(candidate_len),
                 candidate_score,
                 static_cast<unsigned>(candidate_width),
                 static_cast<unsigned>(candidate_height));

        if (!ok) {
            if (candidate_buf != nullptr) {
                heap_caps_free(candidate_buf);
            }
            continue;
        }

        const bool replace_best =
            best_buf == nullptr ||
            candidate_score > best_score ||
            (candidate_score == best_score && candidate_len > best_len);
        if (replace_best) {
            if (best_buf != nullptr) {
                heap_caps_free(best_buf);
            }
            best_buf = candidate_buf;
            best_len = candidate_len;
            best_width = candidate_width;
            best_height = candidate_height;
            best_score = candidate_score;
            best_index = i;
        } else {
            heap_caps_free(candidate_buf);
        }

        if (candidate_count > 1 && i + 1 < candidate_count) {
            vTaskDelay(pdMS_TO_TICKS(CAMERA_DOCUMENT_BURST_INTERVAL_MS));
        }
    }

    if (best_buf == nullptr || best_len == 0) {
        return false;
    }

    if (debug != nullptr) {
        debug->profile = capture_controller_.GetNextCaptureProfile();
        debug->jpeg_len = best_len;
        debug->width = best_width;
        debug->height = best_height;
        debug->sharpness = best_score;
        debug->af_attempted = last_af_attempted_;
        debug->af_focused = last_af_focused_;
        debug->af_raw = last_af_raw_;
        debug->candidate_count = candidate_count;
        debug->selected_candidate = best_index;
    }

    *out_buf = best_buf;
    *out_len = best_len;
    return true;
}

void MossCameraHardwareController::SetDisplayUpdatesSuspended(bool suspended) const {
    if (display_ != nullptr) {
        display_->SuspendUpdates(suspended);
    }
}

void MossCameraHardwareController::RecoverDisplayAfterCameraUse() const {
    if (panel_ == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Recover OLED after camera use");
    vTaskDelay(pdMS_TO_TICKS(CAMERA_DISPLAY_RECOVERY_DELAY_MS));
    if (esp_lcd_panel_reset(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "OLED reset failed during camera recovery");
    }
    if (esp_lcd_panel_init(panel_) != ESP_OK) {
        ESP_LOGW(TAG, "OLED init failed during camera recovery");
    }
    if (esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y) != ESP_OK) {
        ESP_LOGW(TAG, "OLED mirror restore failed during camera recovery");
    }
    if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
        ESP_LOGW(TAG, "OLED power-on failed during camera recovery");
    }
}

