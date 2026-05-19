#include "application_state_controller.h"

#include <esp_timer.h>

#include "assets/lang_config.h"
#include "board.h"
#include "display.h"

void ApplicationStateController::HandleStateChangedEvent(
    DeviceStateMachine& state_machine,
    Protocol* protocol,
    AudioService& audio_service,
    ListeningMode listening_mode,
    bool& play_popup_on_listening,
    int64_t& state_enter_us,
    int& clock_ticks,
    uint32_t& listening_sent_packets,
    uint64_t& listening_sent_bytes) {
    DeviceState new_state = state_machine.GetState();
    state_enter_us = esp_timer_get_time();
    if (new_state == kDeviceStateListening) {
        listening_sent_packets = 0;
        listening_sent_bytes = 0;
    }
    clock_ticks = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    board.OnDeviceStateChanged(new_state);

    switch (new_state) {
    case kDeviceStateUnknown:
    case kDeviceStateIdle:
        display->SetStatus(Lang::Strings::STANDBY);
        display->ClearChatMessages();
        display->SetEmotion("neutral");
        audio_service.EnableVoiceProcessing(false);
        audio_service.EnableWakeWordDetection(true);
        break;
    case kDeviceStateConnecting:
        display->SetStatus(Lang::Strings::CONNECTING);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
        audio_service.EnableWakeWordDetection(false);
        break;
    case kDeviceStateListening:
        display->SetStatus(Lang::Strings::LISTENING);
        display->SetEmotion("neutral");

        if (play_popup_on_listening || !audio_service.IsAudioProcessorRunning()) {
            if (listening_mode == kListeningModeAutoStop) {
                audio_service.WaitForPlaybackQueueEmpty();
            }
            if (protocol != nullptr) {
                protocol->SendStartListening(listening_mode);
            }
            audio_service.EnableVoiceProcessing(true);
        } else {
            if (protocol != nullptr) {
                protocol->SendStartListening(listening_mode);
            }
        }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
        audio_service.EnableWakeWordDetection(audio_service.IsAfeWakeWord());
#else
        audio_service.EnableWakeWordDetection(false);
#endif

        if (play_popup_on_listening) {
            play_popup_on_listening = false;
            audio_service.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        break;
    case kDeviceStateSpeaking:
        display->SetStatus(Lang::Strings::SPEAKING);
        audio_service.EnsureOutputReady();
        if (listening_mode == kListeningModeRealtime) {
            audio_service.EnableVoiceProcessing(true);
            audio_service.EnableWakeWordDetection(audio_service.IsAfeWakeWord());
        } else {
            audio_service.EnableVoiceProcessing(false);
            audio_service.EnableWakeWordDetection(audio_service.IsAfeWakeWord());
        }
        audio_service.ResetDecoder();
        break;
    case kDeviceStateWifiConfiguring:
        audio_service.EnableVoiceProcessing(false);
        audio_service.EnableWakeWordDetection(false);
        break;
    default:
        break;
    }
}

