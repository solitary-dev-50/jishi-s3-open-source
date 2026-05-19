#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <inttypes.h>
#include <initializer_list>
#include <optional>
#include <sstream>

#define TAG "Application"

namespace {
constexpr int kSteadyAudioYieldDelayMs = 2;
constexpr int kSteadyAudioPacketsPerSlice = 2;
constexpr uint32_t kRealtimeInterruptCooldownMs = 600;
TickType_t DelayTicksAtLeastOne(int delay_ms) {
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks > 0 ? ticks : 1;
}

void LogInternalHeapSnapshot(const char* stage) {
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "Heap snapshot [%s]: internal_free=%u internal_largest=%u internal_min=%u",
             stage,
             static_cast<unsigned>(free_internal),
             static_cast<unsigned>(largest_internal),
             static_cast<unsigned>(min_internal));
}

}


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}


void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    LogInternalHeapSnapshot("app init after audio start");

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
    LogInternalHeapSnapshot("app init after mcp tools");

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            const auto current_state = GetDeviceState();
            const bool listening_send = current_state == kDeviceStateListening;
            const bool allow_send =
                current_state == kDeviceStateListening ||
                (current_state == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime);
            int sent_since_yield = 0;
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!allow_send) {
                    continue;
                }
                if (protocol_ && !protocol_->SendAudio(*packet)) {
                    const auto queue_diag = audio_service_.GetQueueDiagnostics();
                    const int64_t now_us = esp_timer_get_time();
                    const uint32_t state_age_ms = state_enter_us_ > 0 && now_us >= state_enter_us_
                        ? static_cast<uint32_t>((now_us - state_enter_us_) / 1000)
                        : 0;
                    ESP_LOGW(TAG,
                        "SendAudio failed in app loop: ts=%lu payload=%u remaining_send=%u pending_encode=%u state=%d state_age_ms=%u listening_sent_packets=%lu listening_sent_bytes=%llu send_hw=%u encode_hw=%u send_enq=%lu send_pop=%lu",
                        static_cast<unsigned long>(packet->timestamp),
                        static_cast<unsigned>(packet->payload.size()),
                        static_cast<unsigned>(queue_diag.send_queue_size),
                        static_cast<unsigned>(queue_diag.encode_queue_size),
                        static_cast<int>(GetDeviceState()),
                        static_cast<unsigned>(state_age_ms),
                        static_cast<unsigned long>(listening_sent_packets_),
                        static_cast<unsigned long long>(listening_sent_bytes_),
                        static_cast<unsigned>(queue_diag.send_queue_high_watermark),
                        static_cast<unsigned>(queue_diag.encode_queue_high_watermark),
                        static_cast<unsigned long>(queue_diag.send_packets_enqueued),
                        static_cast<unsigned long>(queue_diag.send_packets_popped));
                    break;
                }
                if (listening_send) {
                    ++listening_sent_packets_;
                    listening_sent_bytes_ += packet->payload.size();
                }
                if (listening_send && ++sent_since_yield >= kSteadyAudioPacketsPerSlice) {
                    sent_since_yield = 0;
                    vTaskDelay(DelayTicksAtLeastOne(kSteadyAudioYieldDelayMs));
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            HandleVadChangeEvent();
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    LogInternalHeapSnapshot("activation done before ota release");
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    LogInternalHeapSnapshot("activation done after ota release");
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    ApplicationActivationController::Callbacks callbacks{
        .alert = [this](const char* status,
                        const char* message,
                        const char* emotion,
                        const std::string_view& sound) {
            Alert(status, message, emotion, sound);
        },
        .set_device_state = [this](DeviceState state) {
            SetDeviceState(state);
        },
        .get_device_state = [this]() {
            return GetDeviceState();
        },
        .upgrade_firmware = [this](const std::string& url, const std::string& version) {
            return UpgradeFirmware(url, version);
        },
    };

    activation_controller_.RunActivationTask(
        ota_,
        assets_version_checked_,
        event_group_,
        MAIN_EVENT_ACTIVATION_DONE,
        audio_service_,
        callbacks,
        [this]() {
            InitializeProtocol();
        });
}

void Application::InitializeProtocol() {
    auto display = Board::GetInstance().GetDisplay();
    auto codec = Board::GetInstance().GetAudioCodec();

    ApplicationProtocolController::Callbacks callbacks{
        .dismiss_alert = [this]() {
            DismissAlert();
        },
        .on_network_error = [this](const std::string& message) {
            last_error_message_ = message;
            xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
        },
        .on_incoming_audio = [this](std::unique_ptr<AudioStreamPacket> packet) {
            if (GetDeviceState() == kDeviceStateSpeaking) {
                audio_service_.PushPacketToDecodeQueue(std::move(packet));
            }
        },
        .on_audio_channel_opened = []() {},
        .on_audio_channel_closed = [this]() {
            Schedule([this]() {
                auto display = Board::GetInstance().GetDisplay();
                display->SetChatMessage("system", "");
                SetDeviceState(kDeviceStateIdle);
            });
        },
        .on_incoming_json = [this, display](const cJSON* root) {
            ApplicationProtocolMessageController::Callbacks callbacks{
                .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
                .get_device_state = [this]() { return GetDeviceState(); },
                .get_listening_mode = [this]() { return listening_mode_; },
                .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
                .request_listening_mode = [this](ListeningMode mode) {
                    if (!protocol_) {
                        return;
                    }
                    if (!protocol_->IsAudioChannelOpened() && !protocol_->OpenAudioChannel()) {
                        ESP_LOGW(TAG, "Failed to open audio channel for homework read mode");
                        return;
                    }
                    SetListeningMode(mode);
                },
                .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
                .arm_local_servo_cloud_suppression = [this](uint32_t duration_ms) {
                    ArmLocalServoCloudSuppression(duration_ms);
                },
                .is_local_servo_cloud_suppressed = [this]() {
                    return IsLocalServoCloudSuppressed();
                },
                .get_protocol_session_id = [this]() -> std::string {
                    return protocol_ ? protocol_->session_id() : std::string();
                },
                .reset_protocol = [this]() { ResetProtocol(); },
                .reboot = [this]() { Reboot(); },
                .play_sound = [this](const std::string_view& sound) {
                    PlaySound(sound);
                },
                .alert = [this](const char* status,
                                const char* message,
                                const char* emotion,
                                const std::string_view& sound) {
                    Alert(status, message, emotion, sound);
                },
            };
            protocol_message_controller_.HandleIncomingJson(root, display, callbacks);
        },
    };

    protocol_controller_.InitializeProtocol(protocol_, *ota_, codec, callbacks);
    LogInternalHeapSnapshot("activation after protocol start");
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    ApplicationAudioSessionController::Callbacks callbacks{
        .get_device_state = [this]() { return GetDeviceState(); },
        .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
        .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
        .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
        .get_default_listening_mode = [this]() { return GetDefaultListeningMode(); },
        .set_listening_mode = [this](ListeningMode mode) { SetListeningMode(mode); },
    };
    audio_session_controller_.HandleToggleChatEvent(protocol_.get(), audio_service_, callbacks);
}

void Application::HandleStartListeningEvent() {
    ApplicationAudioSessionController::Callbacks callbacks{
        .get_device_state = [this]() { return GetDeviceState(); },
        .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
        .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
        .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
        .get_default_listening_mode = [this]() { return GetDefaultListeningMode(); },
        .set_listening_mode = [this](ListeningMode mode) { SetListeningMode(mode); },
    };
    audio_session_controller_.HandleStartListeningEvent(protocol_.get(), audio_service_, callbacks);
}

void Application::HandleStopListeningEvent() {
    protocol_message_controller_.NotifyStopListening(Board::GetInstance().GetDisplay());
    ApplicationAudioSessionController::Callbacks callbacks{
        .get_device_state = [this]() { return GetDeviceState(); },
        .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
        .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
        .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
        .get_default_listening_mode = [this]() { return GetDefaultListeningMode(); },
        .set_listening_mode = [this](ListeningMode mode) { SetListeningMode(mode); },
    };
    audio_session_controller_.HandleStopListeningEvent(protocol_.get(), audio_service_, callbacks);
}

void Application::HandleWakeWordDetectedEvent() {
    ApplicationAudioSessionController::Callbacks callbacks{
        .get_device_state = [this]() { return GetDeviceState(); },
        .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
        .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
        .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
        .get_default_listening_mode = [this]() { return GetDefaultListeningMode(); },
        .set_listening_mode = [this](ListeningMode mode) { SetListeningMode(mode); },
    };
    audio_session_controller_.HandleWakeWordDetectedEvent(
        protocol_.get(), audio_service_, play_popup_on_listening_, callbacks);
}

void Application::HandleVadChangeEvent() {
    const auto state = GetDeviceState();
    if (state == kDeviceStateListening) {
        auto led = Board::GetInstance().GetLed();
        led->OnStateChanged();
        return;
    }

    if (state != kDeviceStateSpeaking) {
        return;
    }
    if (listening_mode_ != kListeningModeRealtime || aec_mode_ == kAecOff) {
        return;
    }

    if (!audio_service_.IsVoiceDetected()) {
        return;
    }

    const uint32_t input_level = audio_service_.GetInputLevel();
    const uint32_t output_level = audio_service_.GetOutputLevel();
    const uint32_t state_age_ms = state_enter_us_ > 0
                                      ? static_cast<uint32_t>((esp_timer_get_time() - state_enter_us_) / 1000)
                                      : 0;
    if (state_age_ms < 700) {
        return;
    }
    if (input_level < 24) {
        return;
    }
    if (output_level > 0 && input_level + 12 < output_level) {
        return;
    }

    const uint32_t now_ms = static_cast<uint32_t>(esp_log_timestamp());
    if (now_ms - last_realtime_interrupt_ms_ < kRealtimeInterruptCooldownMs) {
        return;
    }
    last_realtime_interrupt_ms_ = now_ms;

    ESP_LOGW(TAG,
             "Realtime interruption detected: input=%lu output=%lu state_age=%lums -> abort speaking",
             static_cast<unsigned long>(input_level),
             static_cast<unsigned long>(output_level),
             static_cast<unsigned long>(state_age_ms));
    audio_service_.ResetDecoder();
    AbortSpeaking(kAbortReasonNone);
    play_popup_on_listening_ = true;
    SetDeviceState(kDeviceStateListening);
}

void Application::HandleStateChangedEvent() {
    state_controller_.HandleStateChangedEvent(
        state_machine_,
        protocol_.get(),
        audio_service_,
        listening_mode_,
        play_popup_on_listening_,
        state_enter_us_,
        clock_ticks_,
        listening_sent_packets_,
        listening_sent_bytes_);
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::ArmLocalServoCloudSuppression(uint32_t duration_ms) {
    local_servo_cloud_suppress_until_ms_ =
        static_cast<uint64_t>(esp_log_timestamp()) + static_cast<uint64_t>(duration_ms);
    ESP_LOGW(TAG,
             "Local servo cloud suppression armed: duration=%lums until=%" PRIu64 "ms",
             static_cast<unsigned long>(duration_ms),
             local_servo_cloud_suppress_until_ms_);
}

bool Application::IsLocalServoCloudSuppressed() const {
    if (local_servo_cloud_suppress_until_ms_ == 0) {
        return false;
    }
    const uint64_t now_ms = static_cast<uint64_t>(esp_log_timestamp());
    return now_ms < local_servo_cloud_suppress_until_ms_;
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}


void Application::Reboot() {
    runtime_controller_.Reboot(protocol_, audio_service_);
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    return runtime_controller_.UpgradeFirmware(
        protocol_,
        audio_service_,
        url,
        version,
        [this](const char* status, const char* message, const char* emotion, const std::string_view& sound) {
            Alert(status, message, emotion, sound);
        },
        [this](DeviceState state) { SetDeviceState(state); },
        [this]() { Reboot(); },
        [this](std::function<void()>&& task) { Schedule(std::move(task)); });
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        ApplicationAudioSessionController::Callbacks callbacks{
            .get_device_state = [this]() { return GetDeviceState(); },
            .set_device_state = [this](DeviceState state) { SetDeviceState(state); },
            .schedule = [this](std::function<void()>&& task) { Schedule(std::move(task)); },
            .abort_speaking = [this](AbortReason reason) { AbortSpeaking(reason); },
            .get_default_listening_mode = [this]() { return GetDefaultListeningMode(); },
            .set_listening_mode = [this](ListeningMode mode) { SetListeningMode(mode); },
        };

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, wake_word, callbacks]() mutable {
                audio_session_controller_.ContinueWakeWordInvoke(
                    protocol_.get(), audio_service_, wake_word, play_popup_on_listening_, callbacks);
            });
            return;
        }

        audio_session_controller_.ContinueWakeWordInvoke(
            protocol_.get(), audio_service_, wake_word, play_popup_on_listening_, callbacks);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    return runtime_controller_.CanEnterSleepMode(GetDeviceState(), protocol_.get(), audio_service_);
}

void Application::SendMcpMessage(const std::string& payload) {
    runtime_controller_.SendMcpMessage(
        [this]() { return protocol_.get(); },
        [this](std::function<void()>&& task) { Schedule(std::move(task)); },
        payload);
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}


void Application::ResetProtocol() {
    runtime_controller_.ResetProtocol(
        protocol_,
        [this](std::function<void()>&& task) { Schedule(std::move(task)); });
}

