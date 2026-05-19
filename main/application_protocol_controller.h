#ifndef APPLICATION_PROTOCOL_CONTROLLER_H
#define APPLICATION_PROTOCOL_CONTROLLER_H

#include <functional>
#include <memory>
#include <string>

#include <cJSON.h>

#include "audio_service.h"
#include "ota.h"
#include "protocol.h"

class AudioCodec;

class ApplicationProtocolController {
public:
    struct Callbacks {
        std::function<void()> dismiss_alert;
        std::function<void(const std::string& message)> on_network_error;
        std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio;
        std::function<void()> on_audio_channel_opened;
        std::function<void()> on_audio_channel_closed;
        std::function<void(const cJSON* root)> on_incoming_json;
    };

    void InitializeProtocol(std::unique_ptr<Protocol>& protocol,
                            Ota& ota,
                            AudioCodec* codec,
                            const Callbacks& callbacks);
};

#endif  // APPLICATION_PROTOCOL_CONTROLLER_H
