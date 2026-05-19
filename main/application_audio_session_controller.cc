#include "application_audio_session_controller.h"

#include <esp_log.h>

#include "assets/lang_config.h"
#include "board.h"

namespace {

constexpr const char* TAG = "AppAudioSession";
constexpr int kWakeWordPacketPaceMs = 8;
constexpr int kWakeWordPacketRetryBackoffMs = 20;
constexpr int kWakeWordPacketMaxRetries = 2;

TickType_t DelayTicksAtLeastOne(int delay_ms) {
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    return ticks > 0 ? ticks : 1;
}

}  // namespace

void ApplicationAudioSessionController::HandleToggleChatEvent(Protocol* protocol,
                                                              AudioService& audio_service,
                                                              const Callbacks& callbacks) {
    auto state = callbacks.get_device_state();

    if (state == kDeviceStateActivating) {
        callbacks.set_device_state(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service.EnableAudioTesting(true);
        callbacks.set_device_state(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service.EnableAudioTesting(false);
        callbacks.set_device_state(kDeviceStateWifiConfiguring);
        return;
    }

    if (protocol == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = callbacks.get_default_listening_mode();
        if (!protocol->IsAudioChannelOpened()) {
            callbacks.set_device_state(kDeviceStateConnecting);
            callbacks.schedule([this, protocol, mode, callbacks]() {
                ContinueOpenAudioChannel(protocol, mode, callbacks);
            });
            return;
        }
        callbacks.set_listening_mode(mode);
    } else if (state == kDeviceStateSpeaking) {
        callbacks.abort_speaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol->CloseAudioChannel();
    }
}

void ApplicationAudioSessionController::ContinueOpenAudioChannel(Protocol* protocol,
                                                                 ListeningMode mode,
                                                                 const Callbacks& callbacks) {
    if (callbacks.get_device_state() != kDeviceStateConnecting || protocol == nullptr) {
        return;
    }

    if (!protocol->IsAudioChannelOpened()) {
        if (!protocol->OpenAudioChannel()) {
            return;
        }
    }

    callbacks.set_listening_mode(mode);
}

void ApplicationAudioSessionController::HandleStartListeningEvent(Protocol* protocol,
                                                                  AudioService& audio_service,
                                                                  const Callbacks& callbacks) {
    auto state = callbacks.get_device_state();

    if (state == kDeviceStateActivating) {
        callbacks.set_device_state(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service.EnableAudioTesting(true);
        callbacks.set_device_state(kDeviceStateAudioTesting);
        return;
    }

    if (protocol == nullptr) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol->IsAudioChannelOpened()) {
            callbacks.set_device_state(kDeviceStateConnecting);
            callbacks.schedule([this, protocol, callbacks]() {
                ContinueOpenAudioChannel(protocol, kListeningModeManualStop, callbacks);
            });
            return;
        }
        callbacks.set_listening_mode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        callbacks.abort_speaking(kAbortReasonNone);
        callbacks.set_listening_mode(kListeningModeManualStop);
    }
}

void ApplicationAudioSessionController::HandleStopListeningEvent(Protocol* protocol,
                                                                 AudioService& audio_service,
                                                                 const Callbacks& callbacks) {
    auto state = callbacks.get_device_state();

    if (state == kDeviceStateAudioTesting) {
        audio_service.EnableAudioTesting(false);
        callbacks.set_device_state(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol != nullptr) {
            protocol->SendStopListening();
        }
        callbacks.set_device_state(kDeviceStateIdle);
    }
}

void ApplicationAudioSessionController::HandleWakeWordDetectedEvent(Protocol* protocol,
                                                                    AudioService& audio_service,
                                                                    bool& play_popup_on_listening,
                                                                    const Callbacks& callbacks) {
    if (protocol == nullptr) {
        return;
    }

    auto& board = Board::GetInstance();
    auto state = callbacks.get_device_state();
    auto wake_word = audio_service.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), static_cast<int>(state));

    if (state == kDeviceStateIdle) {
        board.OnWakeWordDetected();
        audio_service.EncodeWakeWord();
        auto encoded_wake_word = audio_service.GetLastWakeWord();

        if (!protocol->IsAudioChannelOpened()) {
            callbacks.set_device_state(kDeviceStateConnecting);
            callbacks.schedule([this, protocol, &audio_service, encoded_wake_word, &play_popup_on_listening, callbacks]() {
                ContinueWakeWordInvoke(protocol, audio_service, encoded_wake_word, play_popup_on_listening, callbacks);
            });
            return;
        }
        ContinueWakeWordInvoke(protocol, audio_service, encoded_wake_word, play_popup_on_listening, callbacks);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        callbacks.abort_speaking(kAbortReasonWakeWordDetected);
        while (audio_service.PopPacketFromSendQueue()) {
        }

        if (state == kDeviceStateListening) {
            protocol->SendStartListening(callbacks.get_default_listening_mode());
            audio_service.ResetDecoder();
            audio_service.PlaySound(Lang::Sounds::OGG_POPUP);
            audio_service.EnableWakeWordDetection(true);
        } else {
            play_popup_on_listening = true;
            callbacks.set_listening_mode(callbacks.get_default_listening_mode());
        }
    } else if (state == kDeviceStateActivating) {
        callbacks.set_device_state(kDeviceStateIdle);
    }
}

void ApplicationAudioSessionController::ContinueWakeWordInvoke(Protocol* protocol,
                                                               AudioService& audio_service,
                                                               const std::string& wake_word,
                                                               bool& play_popup_on_listening,
                                                               const Callbacks& callbacks) {
    if (callbacks.get_device_state() != kDeviceStateConnecting || protocol == nullptr) {
        return;
    }

    if (!protocol->IsAudioChannelOpened()) {
        if (!protocol->OpenAudioChannel()) {
            audio_service.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    SendWakeWordPacketsWithPacing(protocol, audio_service);
    protocol->SendWakeWordDetected(wake_word);
    play_popup_on_listening = true;
    callbacks.set_listening_mode(callbacks.get_default_listening_mode());
#else
    play_popup_on_listening = true;
    callbacks.set_listening_mode(callbacks.get_default_listening_mode());
#endif
}

bool ApplicationAudioSessionController::SendWakeWordPacketsWithPacing(Protocol* protocol,
                                                                      AudioService& audio_service) {
    if (protocol == nullptr) {
        return false;
    }

    const TickType_t pace_ticks = DelayTicksAtLeastOne(kWakeWordPacketPaceMs);
    const TickType_t retry_ticks = DelayTicksAtLeastOne(kWakeWordPacketRetryBackoffMs);
    int sent_packets = 0;
    int retried_packets = 0;
    int dropped_packets = 0;

    while (auto packet = audio_service.PopWakeWordPacket()) {
        AudioStreamPacket original_packet = *packet;
        bool sent = false;

        for (int attempt = 0; attempt <= kWakeWordPacketMaxRetries; ++attempt) {
            if (protocol->SendAudio(original_packet)) {
                sent = true;
                ++sent_packets;
                break;
            }

            if (attempt == kWakeWordPacketMaxRetries) {
                break;
            }

            ++retried_packets;
            vTaskDelay(retry_ticks);
        }

        if (!sent) {
            ++dropped_packets;
        }

        vTaskDelay(pace_ticks);
    }

    if (dropped_packets > 0) {
        ESP_LOGW(TAG,
                 "Wake word data paced send: sent=%d retried=%d dropped=%d",
                 sent_packets,
                 retried_packets,
                 dropped_packets);
        return false;
    }

    ESP_LOGI(TAG,
             "Wake word data paced send: sent=%d retried=%d dropped=%d",
             sent_packets,
             retried_packets,
             dropped_packets);
    return true;
}

