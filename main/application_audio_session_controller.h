#ifndef APPLICATION_AUDIO_SESSION_CONTROLLER_H
#define APPLICATION_AUDIO_SESSION_CONTROLLER_H

#include <functional>
#include <string>

#include "audio_service.h"
#include "protocol.h"

class ApplicationAudioSessionController {
public:
    struct Callbacks {
        std::function<DeviceState()> get_device_state;
        std::function<void(DeviceState state)> set_device_state;
        std::function<void(std::function<void()>&& task)> schedule;
        std::function<void(AbortReason reason)> abort_speaking;
        std::function<ListeningMode()> get_default_listening_mode;
        std::function<void(ListeningMode mode)> set_listening_mode;
    };

    void HandleToggleChatEvent(Protocol* protocol,
                               AudioService& audio_service,
                               const Callbacks& callbacks);
    void ContinueOpenAudioChannel(Protocol* protocol,
                                  ListeningMode mode,
                                  const Callbacks& callbacks);
    void HandleStartListeningEvent(Protocol* protocol,
                                   AudioService& audio_service,
                                   const Callbacks& callbacks);
    void HandleStopListeningEvent(Protocol* protocol,
                                  AudioService& audio_service,
                                  const Callbacks& callbacks);
    void HandleWakeWordDetectedEvent(Protocol* protocol,
                                     AudioService& audio_service,
                                     bool& play_popup_on_listening,
                                     const Callbacks& callbacks);
    void ContinueWakeWordInvoke(Protocol* protocol,
                                AudioService& audio_service,
                                const std::string& wake_word,
                                bool& play_popup_on_listening,
                                const Callbacks& callbacks);
    bool SendWakeWordPacketsWithPacing(Protocol* protocol, AudioService& audio_service);
};

#endif  // APPLICATION_AUDIO_SESSION_CONTROLLER_H

