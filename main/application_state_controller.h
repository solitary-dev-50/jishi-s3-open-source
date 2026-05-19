#ifndef APPLICATION_STATE_CONTROLLER_H
#define APPLICATION_STATE_CONTROLLER_H

#include <cstdint>

#include "audio_service.h"
#include "device_state_machine.h"
#include "protocol.h"

class ApplicationStateController {
public:
    void HandleStateChangedEvent(DeviceStateMachine& state_machine,
                                 Protocol* protocol,
                                 AudioService& audio_service,
                                 ListeningMode listening_mode,
                                 bool& play_popup_on_listening,
                                 int64_t& state_enter_us,
                                 int& clock_ticks,
                                 uint32_t& listening_sent_packets,
                                 uint64_t& listening_sent_bytes);
};

#endif  // APPLICATION_STATE_CONTROLLER_H

