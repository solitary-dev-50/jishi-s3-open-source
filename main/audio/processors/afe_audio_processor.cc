#include "afe_audio_processor.h"
#include <esp_log.h>
#include <esp_heap_caps.h>

#define PROCESSOR_RUNNING 0x01

#define TAG "AfeAudioProcessor"

namespace {
constexpr uint32_t kAudioProcessorStackBytes = 4096;
constexpr uint32_t kFetchIssueLogIntervalMs = 10000;
constexpr int kAfeFetchNoDataRet = ESP_FAIL;

StackType_t* AllocateTaskStack(const char* task_name, uint32_t stack_size_bytes) {
    auto* stack = static_cast<StackType_t*>(heap_caps_malloc(stack_size_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (stack == nullptr) {
        ESP_LOGW(TAG, "Failed to allocate %s stack in PSRAM, fallback to internal SRAM", task_name);
        stack = static_cast<StackType_t*>(heap_caps_malloc(stack_size_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (stack == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate stack for %s (%u bytes)", task_name, static_cast<unsigned>(stack_size_bytes));
    }
    return stack;
}

StaticTask_t* AllocateTaskBuffer(const char* task_name) {
    auto* buffer = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate task buffer for %s", task_name);
    }
    return buffer;
}

bool ShouldLogFetchIssue(int ret_value, int64_t& last_log_ms, int& last_error_code, uint32_t& repeat_count) {
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (ret_value == last_error_code && (now_ms - last_log_ms) < kFetchIssueLogIntervalMs) {
        ++repeat_count;
        return false;
    }

    repeat_count = 0;
    last_error_code = ret_value;
    last_log_ms = now_ms;
    return true;
}
}

AfeAudioProcessor::AfeAudioProcessor()
    : afe_data_(nullptr) {
    event_group_ = xEventGroupCreate();
}

void AfeAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;

    // Pre-allocate output buffer capacity
    output_buffer_.reserve(frame_samples_);

    int ref_num = codec_->input_reference() ? 1 : 0;

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }

    srmodel_list_t *models;
    if (models_list == nullptr) {
        models = esp_srmodel_init("model");
    } else {
        models = models_list;
    }

    char* ns_model_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char* vad_model_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), NULL, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
    afe_config->vad_mode = VAD_MODE_0;
    afe_config->vad_min_noise_ms = 100;
    if (vad_model_name != nullptr) {
        afe_config->vad_model_name = vad_model_name;
    }

    if (ns_model_name != nullptr) {
        afe_config->ns_init = true;
        afe_config->ns_model_name = ns_model_name;
        afe_config->afe_ns_mode = AFE_NS_MODE_NET;
    } else {
        afe_config->ns_init = false;
    }

    afe_config->agc_init = false;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

#ifdef CONFIG_USE_DEVICE_AEC
    afe_config->aec_init = true;
    afe_config->vad_init = false;
#else
    afe_config->aec_init = false;
    afe_config->vad_init = true;
#endif

    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

    audio_processor_task_stack_ = AllocateTaskStack("audio_communication", kAudioProcessorStackBytes);
    audio_processor_task_buffer_ = AllocateTaskBuffer("audio_communication");
    if (audio_processor_task_stack_ == nullptr || audio_processor_task_buffer_ == nullptr) {
        return;
    }

    audio_processor_task_handle_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (AfeAudioProcessor*)arg;
        this_->AudioProcessorTask();
        this_->audio_processor_task_handle_ = nullptr;
        vTaskDelete(NULL);
    }, "audio_communication", kAudioProcessorStackBytes, this, 3, audio_processor_task_stack_, audio_processor_task_buffer_);
    if (audio_processor_task_handle_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio_communication task");
    }
}

AfeAudioProcessor::~AfeAudioProcessor() {
    audio_processor_task_shutdown_ = true;
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
    WaitForProcessorTaskExit();

    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }
    if (audio_processor_task_stack_ != nullptr) {
        heap_caps_free(audio_processor_task_stack_);
    }
    if (audio_processor_task_buffer_ != nullptr) {
        heap_caps_free(audio_processor_task_buffer_);
    }
    vEventGroupDelete(event_group_);
}

size_t AfeAudioProcessor::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_);
}

void AfeAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (afe_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!IsRunning()) {
        return;
    }
    input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    size_t chunk_size = afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
    while (input_buffer_.size() >= chunk_size) {
        afe_iface_->feed(afe_data_, input_buffer_.data());
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunk_size);
    }
}

void AfeAudioProcessor::Start() {
    xEventGroupSetBits(event_group_, PROCESSOR_RUNNING);
}

void AfeAudioProcessor::Stop() {
    xEventGroupClearBits(event_group_, PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
    input_buffer_.clear();
}

bool AfeAudioProcessor::IsRunning() {
    return xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING;
}

void AfeAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

void AfeAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

void AfeAudioProcessor::AudioProcessorTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio communication task started, feed size: %d fetch size: %d",
        feed_size, fetch_size);

    while (!audio_processor_task_shutdown_) {
        auto bits = xEventGroupWaitBits(event_group_, PROCESSOR_RUNNING, pdFALSE, pdTRUE, pdMS_TO_TICKS(100));
        if (audio_processor_task_shutdown_) {
            break;
        }
        if ((bits & PROCESSOR_RUNNING) == 0) {
            continue;
        }

        auto res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
        if (audio_processor_task_shutdown_) {
            break;
        }
        if ((xEventGroupGetBits(event_group_) & PROCESSOR_RUNNING) == 0) {
            continue;
        }
        if (res == nullptr) {
            continue;
        }
        if (res->ret_value == kAfeFetchNoDataRet) {
            ++fetch_issue_repeat_count_;
            last_fetch_error_code_ = res->ret_value;
            continue;
        }
        if (res->ret_value < 0) {
            if (ShouldLogFetchIssue(res->ret_value, last_fetch_issue_log_ms_, last_fetch_error_code_, fetch_issue_repeat_count_)) {
                ESP_LOGW(TAG, "AFE fetch returned unexpected %d while running; suppressing repeated logs", res->ret_value);
            }
            continue;
        }

        fetch_issue_repeat_count_ = 0;
        last_fetch_error_code_ = 0;

        // VAD state change
        if (vad_state_change_callback_) {
            if (res->vad_state == VAD_SPEECH && !is_speaking_) {
                is_speaking_ = true;
                vad_state_change_callback_(true);
            } else if (res->vad_state == VAD_SILENCE && is_speaking_) {
                is_speaking_ = false;
                vad_state_change_callback_(false);
            }
        }

        if (output_callback_) {
            size_t samples = res->data_size / sizeof(int16_t);
            
            // Add data to buffer
            output_buffer_.insert(output_buffer_.end(), res->data, res->data + samples);
            
            // Output complete frames when buffer has enough data
            while (output_buffer_.size() >= frame_samples_) {
                if (output_buffer_.size() == frame_samples_) {
                    // If buffer size equals frame size, move the entire buffer
                    output_callback_(std::move(output_buffer_));
                    output_buffer_.clear();
                    output_buffer_.reserve(frame_samples_);
                } else {
                    // If buffer size exceeds frame size, copy one frame and remove it
                    output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
                    output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
                }
            }
        }
    }

    ESP_LOGW(TAG, "Audio communication task stopped");
}

void AfeAudioProcessor::EnableDeviceAec(bool enable) {
    if (enable) {
#if CONFIG_USE_DEVICE_AEC
        afe_iface_->disable_vad(afe_data_);
        afe_iface_->enable_aec(afe_data_);
#else
        ESP_LOGE(TAG, "Device AEC is not supported");
#endif
    } else {
        afe_iface_->disable_aec(afe_data_);
        afe_iface_->enable_vad(afe_data_);
    }
}

void AfeAudioProcessor::WaitForProcessorTaskExit(uint32_t timeout_ms) {
    if (audio_processor_task_handle_ == nullptr) {
        return;
    }

    const TickType_t delay_ticks = pdMS_TO_TICKS(10);
    uint32_t max_rounds = timeout_ms / 10;
    if (max_rounds == 0) {
        max_rounds = 1;
    }
    for (uint32_t i = 0; i < max_rounds; ++i) {
        if (audio_processor_task_handle_ == nullptr) {
            return;
        }
        vTaskDelay(delay_ticks);
    }

    ESP_LOGW(TAG, "Timed out waiting for audio_communication task to exit");
}
