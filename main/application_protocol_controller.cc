#include "application_protocol_controller.h"

#include <esp_log.h>

#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"

namespace {

constexpr const char* TAG = "AppProtocol";

}  // namespace

void ApplicationProtocolController::InitializeProtocol(std::unique_ptr<Protocol>& protocol,
                                                       Ota& ota,
                                                       AudioCodec* codec,
                                                       const Callbacks& callbacks) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota.HasMqttConfig()) {
        protocol = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol = std::make_unique<MqttProtocol>();
    }

    protocol->OnConnected([callbacks]() {
        callbacks.dismiss_alert();
    });

    protocol->OnNetworkError([callbacks](const std::string& message) {
        callbacks.on_network_error(message);
    });

    protocol->OnIncomingAudio([callbacks](std::unique_ptr<AudioStreamPacket> packet) {
        callbacks.on_incoming_audio(std::move(packet));
    });

    protocol->OnAudioChannelOpened([callbacks, codec, &board, &protocol]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (codec != nullptr && protocol != nullptr &&
            protocol->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                     protocol->server_sample_rate(),
                     codec->output_sample_rate());
        }
        callbacks.on_audio_channel_opened();
    });

    protocol->OnAudioChannelClosed([callbacks, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        callbacks.on_audio_channel_closed();
    });

    protocol->OnIncomingJson([callbacks](const cJSON* root) {
        callbacks.on_incoming_json(root);
    });

    protocol->Start();
}
