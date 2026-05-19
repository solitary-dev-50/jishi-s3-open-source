#include "jishi_camera_session_controller.h"

#include "config.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "MossCamSession";
}

MossCameraSessionController::MossCameraSessionController(std::atomic<bool>& camera_i2c_occupied)
    : camera_i2c_occupied_(camera_i2c_occupied) {}

bool MossCameraSessionController::BeginCaptureSession(bool& lvgl_stopped,
                                                      const Callbacks& callbacks) const {
    lvgl_stopped = false;

    const size_t internal_free = callbacks.get_internal_sram_free();
    if (internal_free < CAMERA_CAPTURE_MIN_INTERNAL_SRAM_BYTES) {
        ESP_LOGW(TAG,
                 "CaptureOnce skipped: low internal SRAM, free=%u largest=%u min=%u threshold=%u",
                 static_cast<unsigned>(internal_free),
                 static_cast<unsigned>(callbacks.get_internal_sram_largest_block()),
                 static_cast<unsigned>(callbacks.get_internal_sram_minimum()),
                 static_cast<unsigned>(CAMERA_CAPTURE_MIN_INTERNAL_SRAM_BYTES));
        return false;
    }

    bool expected = false;
    if (!camera_i2c_occupied_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "CaptureOnce skipped: camera bus already occupied");
        return false;
    }

    if (callbacks.is_document_capture_profile()) {
        callbacks.align_for_document_capture();
    }

    callbacks.set_display_updates_suspended(true);
    if (lvgl_port_stop() != ESP_OK) {
        ESP_LOGW(TAG, "LVGL stop failed before CaptureOnce");
    } else {
        ESP_LOGI(TAG, "LVGL stopped for CaptureOnce");
        lvgl_stopped = true;
    }

    callbacks.force_camera_off_for_shared_i2c_boot();
    vTaskDelay(pdMS_TO_TICKS(20));

    const size_t internal_free_after_quiesce = callbacks.get_internal_sram_free();
    if (internal_free_after_quiesce < CAMERA_CAPTURE_MIN_INTERNAL_SRAM_BYTES) {
        ESP_LOGW(TAG,
                 "CaptureOnce skipped after quiesce: low internal SRAM, free=%u largest=%u min=%u threshold=%u",
                 static_cast<unsigned>(internal_free_after_quiesce),
                 static_cast<unsigned>(callbacks.get_internal_sram_largest_block()),
                 static_cast<unsigned>(callbacks.get_internal_sram_minimum()),
                 static_cast<unsigned>(CAMERA_CAPTURE_MIN_INTERNAL_SRAM_BYTES));
        if (lvgl_stopped) {
            if (lvgl_port_resume() != ESP_OK) {
                ESP_LOGW(TAG, "LVGL resume failed after CaptureOnce preflight abort");
            } else {
                ESP_LOGI(TAG, "LVGL resumed after CaptureOnce preflight abort");
            }
        }
        callbacks.set_display_updates_suspended(false);
        camera_i2c_occupied_.store(false);
        return false;
    }
    return true;
}

void MossCameraSessionController::EndCaptureSession(bool lvgl_stopped,
                                                    Esp32Camera*& camera,
                                                    const Callbacks& callbacks) const {
    if (camera != nullptr) {
        ESP_LOGI(TAG,
                 "CaptureOnce deinit start: internal_free=%u psram_free=%u",
                 static_cast<unsigned>(callbacks.get_internal_sram_free()),
                 static_cast<unsigned>(callbacks.get_psram_free()));
        delete camera;
        camera = nullptr;
    }

    callbacks.power_down_camera_hardware();
    ESP_LOGI(TAG,
             "CaptureOnce deinit complete: internal_free=%u psram_free=%u",
             static_cast<unsigned>(callbacks.get_internal_sram_free()),
             static_cast<unsigned>(callbacks.get_psram_free()));

    callbacks.recover_display_after_camera_use();
    if (lvgl_stopped) {
        if (lvgl_port_resume() != ESP_OK) {
            ESP_LOGW(TAG, "LVGL resume failed after CaptureOnce");
        } else {
            ESP_LOGI(TAG, "LVGL resumed after CaptureOnce");
        }
    }
    callbacks.set_display_updates_suspended(false);
    camera_i2c_occupied_.store(false);
    callbacks.reset_capture_profile();
}

bool MossCameraSessionController::CaptureOnce(Esp32Camera*& camera,
                                              uint8_t** out_buf,
                                              size_t* out_len,
                                              MossCameraCaptureController& capture_controller,
                                              const Callbacks& callbacks) const {
    if (out_buf == nullptr || out_len == nullptr) {
        ESP_LOGE(TAG, "CaptureOnce called with null output pointer");
        return false;
    }

    *out_buf = nullptr;
    *out_len = 0;

    bool lvgl_stopped = false;
    if (!BeginCaptureSession(lvgl_stopped, callbacks)) {
        return false;
    }

    bool ok = false;
    MossCameraCaptureController::CaptureDebugInfo debug_info{};
    const int64_t capture_start_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "CaptureOnce init start: internal_free=%u psram_free=%u",
             static_cast<unsigned>(callbacks.get_internal_sram_free()),
             static_cast<unsigned>(callbacks.get_psram_free()));

    callbacks.initialize_camera();
    if (camera == nullptr) {
        ESP_LOGE(TAG, "CaptureOnce init failed");
        EndCaptureSession(lvgl_stopped, camera, callbacks);
        return false;
    }

    ESP_LOGI(TAG,
             "CaptureOnce init complete: internal_free=%u psram_free=%u",
             static_cast<unsigned>(callbacks.get_internal_sram_free()),
             static_cast<unsigned>(callbacks.get_psram_free()));

    ESP_LOGI(TAG,
             "CaptureOnce grab start: internal_free=%u psram_free=%u",
             static_cast<unsigned>(callbacks.get_internal_sram_free()),
             static_cast<unsigned>(callbacks.get_psram_free()));
    ok = callbacks.capture_latest_frame(out_buf, out_len, &debug_info);
    debug_info.capture_duration_ms = static_cast<uint32_t>((esp_timer_get_time() - capture_start_us) / 1000);
    ESP_LOGI(TAG,
             "CaptureOnce grab %s: internal_free=%u psram_free=%u len=%u sharpness=%.2f candidate=%d/%d duration=%ums",
             ok ? "success" : "failed",
             static_cast<unsigned>(callbacks.get_internal_sram_free()),
             static_cast<unsigned>(callbacks.get_psram_free()),
             static_cast<unsigned>(*out_len),
             debug_info.sharpness,
             debug_info.selected_candidate + 1,
             debug_info.candidate_count,
             static_cast<unsigned>(debug_info.capture_duration_ms));

    EndCaptureSession(lvgl_stopped, camera, callbacks);
    if (ok) {
        capture_controller.StoreLastCaptureSnapshot(*out_buf, *out_len, debug_info);
    }
    return ok;
}

