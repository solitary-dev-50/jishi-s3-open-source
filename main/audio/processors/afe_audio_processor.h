#ifndef AFE_AUDIO_PROCESSOR_H
#define AFE_AUDIO_PROCESSOR_H

#include <esp_afe_sr_models.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "audio_processor.h"
#include "audio_codec.h"

class AfeAudioProcessor : public AudioProcessor {
public:
    AfeAudioProcessor();
    ~AfeAudioProcessor();

    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;
    void EnableDeviceAec(bool enable) override;

private:
    EventGroupHandle_t event_group_ = nullptr;
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    bool is_speaking_ = false;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
    std::vector<int16_t> output_buffer_;
    TaskHandle_t audio_processor_task_handle_ = nullptr;
    StaticTask_t* audio_processor_task_buffer_ = nullptr;
    StackType_t* audio_processor_task_stack_ = nullptr;
    bool audio_processor_task_shutdown_ = false;
    int64_t last_fetch_issue_log_ms_ = 0;
    int last_fetch_error_code_ = 0;
    uint32_t fetch_issue_repeat_count_ = 0;

    void AudioProcessorTask();
    void WaitForProcessorTaskExit(uint32_t timeout_ms = 2000);
};

#endif 
