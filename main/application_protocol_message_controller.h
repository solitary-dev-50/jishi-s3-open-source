#ifndef APPLICATION_PROTOCOL_MESSAGE_CONTROLLER_H
#define APPLICATION_PROTOCOL_MESSAGE_CONTROLLER_H

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <cJSON.h>

#include "device_state.h"
#include "display.h"
#include "protocol.h"

class ApplicationProtocolMessageController {
public:
    struct Callbacks {
        std::function<void(std::function<void()>&& task)> schedule;
        std::function<DeviceState()> get_device_state;
        std::function<ListeningMode()> get_listening_mode;
        std::function<void(DeviceState state)> set_device_state;
        std::function<void(ListeningMode mode)> request_listening_mode;
        std::function<void(AbortReason reason)> abort_speaking;
        std::function<void(uint32_t duration_ms)> arm_local_servo_cloud_suppression;
        std::function<bool()> is_local_servo_cloud_suppressed;
        std::function<std::string()> get_protocol_session_id;
        std::function<void()> reset_protocol;
        std::function<void()> reboot;
        std::function<void(const std::string_view& sound)> play_sound;
        std::function<void(const char* status,
                           const char* message,
                           const char* emotion,
                           const std::string_view& sound)> alert;
    };

    void HandleIncomingJson(const cJSON* root, Display* display, const Callbacks& callbacks);
    void NotifyStopListening(Display* display);

private:
    enum class HomeworkReadModeState {
        Idle,
        LongListening,
    };

    mutable std::mutex homework_read_mode_mutex_;
    HomeworkReadModeState homework_read_mode_state_ = HomeworkReadModeState::Idle;
    bool homework_read_mode_hint_ = false;
};

#endif  // APPLICATION_PROTOCOL_MESSAGE_CONTROLLER_H

