#ifndef _JISHI_CAMERA_CAPTURE_CONTROLLER_H_
#define _JISHI_CAMERA_CAPTURE_CONTROLLER_H_

#include "board.h"
#include "config.h"

#include <cstdint>
#include <mutex>
#include <string>

class MossCameraCaptureController {
public:
    struct CaptureDebugInfo {
        bool valid = false;
        uint32_t sequence = 0;
        uint32_t timestamp_ms = 0;
        uint32_t capture_duration_ms = 0;
        CameraCaptureProfile profile = CameraCaptureProfile::Default;
        size_t jpeg_len = 0;
        size_t width = 0;
        size_t height = 0;
        float sharpness = -1.0f;
        bool af_attempted = false;
        bool af_focused = false;
        uint8_t af_raw = 0;
        int candidate_count = 0;
        int selected_candidate = -1;
        float candidate_scores[CAMERA_DOCUMENT_BURST_COUNT] = {};
    };

    ~MossCameraCaptureController();

    bool IsDocumentCaptureProfile() const;
    CameraCaptureProfile GetNextCaptureProfile() const;
    void SetNextCaptureProfile(CameraCaptureProfile profile);
    void ResetNextCaptureProfile();

    void StoreLastCaptureSnapshot(const uint8_t* jpeg_buf, size_t jpeg_len, const CaptureDebugInfo& debug);
    std::string GetLastCaptureDebugJson() const;
    bool GetLastCaptureJpeg(std::string& jpeg_data, std::string* mime_type = nullptr) const;

private:
    void ClearLastCaptureSnapshotLocked();

    CameraCaptureProfile next_capture_profile_ = CameraCaptureProfile::Default;
    mutable std::mutex last_capture_mutex_;
    uint8_t* last_capture_jpeg_ = nullptr;
    size_t last_capture_jpeg_len_ = 0;
    uint32_t last_capture_sequence_ = 0;
    CaptureDebugInfo last_capture_debug_{};
};

#endif // _JISHI_CAMERA_CAPTURE_CONTROLLER_H_

