#include "jishi_camera_capture_controller.h"

#include <cstring>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr const char* TAG = "MossCaptureCtrl";
}

MossCameraCaptureController::~MossCameraCaptureController() {
    std::lock_guard<std::mutex> lock(last_capture_mutex_);
    ClearLastCaptureSnapshotLocked();
}

bool MossCameraCaptureController::IsDocumentCaptureProfile() const {
    return next_capture_profile_ == CameraCaptureProfile::Document;
}

CameraCaptureProfile MossCameraCaptureController::GetNextCaptureProfile() const {
    return next_capture_profile_;
}

void MossCameraCaptureController::SetNextCaptureProfile(CameraCaptureProfile profile) {
    next_capture_profile_ = profile;
}

void MossCameraCaptureController::ResetNextCaptureProfile() {
    next_capture_profile_ = CameraCaptureProfile::Default;
}

void MossCameraCaptureController::ClearLastCaptureSnapshotLocked() {
    if (last_capture_jpeg_ != nullptr) {
        heap_caps_free(last_capture_jpeg_);
        last_capture_jpeg_ = nullptr;
    }
    last_capture_jpeg_len_ = 0;
    last_capture_debug_ = CaptureDebugInfo{};
}

void MossCameraCaptureController::StoreLastCaptureSnapshot(const uint8_t* jpeg_buf,
                                                           size_t jpeg_len,
                                                           const CaptureDebugInfo& debug) {
    std::lock_guard<std::mutex> lock(last_capture_mutex_);
    ClearLastCaptureSnapshotLocked();
    if (jpeg_buf == nullptr || jpeg_len == 0) {
        return;
    }
    last_capture_jpeg_ = static_cast<uint8_t*>(heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (last_capture_jpeg_ == nullptr) {
        ESP_LOGW(TAG, "Failed to persist last capture snapshot: len=%u", static_cast<unsigned>(jpeg_len));
        return;
    }
    memcpy(last_capture_jpeg_, jpeg_buf, jpeg_len);
    last_capture_jpeg_len_ = jpeg_len;
    last_capture_debug_ = debug;
    last_capture_debug_.valid = true;
    last_capture_debug_.sequence = ++last_capture_sequence_;
    last_capture_debug_.timestamp_ms = static_cast<uint32_t>(esp_log_timestamp());
}

std::string MossCameraCaptureController::GetLastCaptureDebugJson() const {
    std::lock_guard<std::mutex> lock(last_capture_mutex_);
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "valid", last_capture_debug_.valid);
    if (last_capture_debug_.valid) {
        cJSON_AddNumberToObject(json, "sequence", last_capture_debug_.sequence);
        cJSON_AddNumberToObject(json, "timestamp_ms", last_capture_debug_.timestamp_ms);
        cJSON_AddStringToObject(json, "profile",
                                last_capture_debug_.profile == CameraCaptureProfile::Document ? "document" : "default");
        cJSON_AddNumberToObject(json, "jpeg_len", static_cast<double>(last_capture_debug_.jpeg_len));
        cJSON_AddNumberToObject(json, "width", static_cast<double>(last_capture_debug_.width));
        cJSON_AddNumberToObject(json, "height", static_cast<double>(last_capture_debug_.height));
        cJSON_AddNumberToObject(json, "sharpness", last_capture_debug_.sharpness);
        cJSON_AddNumberToObject(json, "capture_duration_ms", last_capture_debug_.capture_duration_ms);
        cJSON_AddBoolToObject(json, "af_attempted", last_capture_debug_.af_attempted);
        cJSON_AddBoolToObject(json, "af_focused", last_capture_debug_.af_focused);
        cJSON_AddNumberToObject(json, "af_raw", last_capture_debug_.af_raw);
        cJSON_AddNumberToObject(json, "candidate_count", last_capture_debug_.candidate_count);
        cJSON_AddNumberToObject(json, "selected_candidate", last_capture_debug_.selected_candidate);
        cJSON* scores = cJSON_CreateArray();
        for (int i = 0; i < last_capture_debug_.candidate_count && i < CAMERA_DOCUMENT_BURST_COUNT; ++i) {
            cJSON_AddItemToArray(scores, cJSON_CreateNumber(last_capture_debug_.candidate_scores[i]));
        }
        cJSON_AddItemToObject(json, "candidate_scores", scores);
    }
    char* json_str = cJSON_PrintUnformatted(json);
    std::string result(json_str != nullptr ? json_str : "{}");
    if (json_str != nullptr) {
        cJSON_free(json_str);
    }
    cJSON_Delete(json);
    return result;
}

bool MossCameraCaptureController::GetLastCaptureJpeg(std::string& jpeg_data, std::string* mime_type) const {
    std::lock_guard<std::mutex> lock(last_capture_mutex_);
    if (!last_capture_debug_.valid || last_capture_jpeg_ == nullptr || last_capture_jpeg_len_ == 0) {
        if (mime_type != nullptr) {
            *mime_type = "";
        }
        jpeg_data.clear();
        return false;
    }
    jpeg_data.assign(reinterpret_cast<const char*>(last_capture_jpeg_), last_capture_jpeg_len_);
    if (mime_type != nullptr) {
        *mime_type = "image/jpeg";
    }
    return true;
}

