#include "mqtt_protocol.h"
#include "board.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // Initialize reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                if (protocol->mqtt_ != nullptr && protocol->mqtt_->IsConnected()) {
                    ESP_LOGI(TAG, "Skip MQTT reconnect timer because client is already connected");
                    return;
                }
                ESP_LOGI(TAG, "Reconnecting to MQTT server");
                auto alive = protocol->alive_;  // Capture alive flag
                app.Schedule([protocol, alive]() {
                    if (*alive) {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "MqttProtocol deinit");
    
    // Mark as dead first to prevent any pending scheduled tasks from executing
    *alive_ = false;
    
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

void MqttProtocol::ScheduleReconnect(uint32_t delay_ms, const char* reason) {
    if (reconnect_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(reconnect_timer_);
    ESP_LOGI(TAG, "Schedule MQTT reconnect in %u ms: %s",
        static_cast<unsigned>(delay_ms),
        reason != nullptr ? reason : "unspecified");
    esp_timer_start_once(reconnect_timer_, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

void MqttProtocol::ResetAudioTransport(bool notify_closed) {
    bool had_audio_channel = false;
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        had_audio_channel = udp_ != nullptr;
        if (udp_ != nullptr) {
            udp_->Disconnect();
            udp_.reset();
        }
    }
    session_id_.clear();
    remote_sequence_ = 0;
    local_sequence_ = 0;
    audio_channel_open_us_ = 0;
    last_audio_attempt_us_ = 0;
    last_audio_success_us_ = 0;

    if (notify_closed && had_audio_channel && on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

void MqttProtocol::ResetMqttClient(const char* reason, bool notify_audio_closed) {
    ESP_LOGW(TAG, "Reset MQTT client: %s", reason != nullptr ? reason : "unspecified");
    ResetAudioTransport(notify_audio_closed);
    if (mqtt_ != nullptr) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
}

bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "Mqtt client already started");
        ResetMqttClient("restart existing mqtt client", false);
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "MQTT endpoint is not specified");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        ResetAudioTransport(true);
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ScheduleReconnect(MQTT_RECONNECT_INTERVAL_MS, "mqtt disconnected");
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "Failed to parse json message %s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "Message type is invalid");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "Received goodbye message, session_id: %s", session_id ? session_id->valuestring : "null");
            if (session_id == nullptr || session_id_ == session_id->valuestring) {
                auto alive = alive_;  // Capture alive flag
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        // Server initiated goodbye, don't send goodbye back to avoid ping-pong
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "Connecting to endpoint %s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "Failed to connect to endpoint, code=%d", mqtt_->GetLastError());
        ResetMqttClient("connect to endpoint failed", false);
        ScheduleReconnect(MQTT_FAST_RECONNECT_INTERVAL_MS, "connect to endpoint failed");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "Connected to endpoint");
    return true;
}

bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "Failed to publish message: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

bool MqttProtocol::SendAudio(const AudioStreamPacket& packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }
    const int64_t now_us = esp_timer_get_time();
    ++audio_send_attempts_;

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet.payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet.timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet.payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&aes_ctx_, packet.payload.size(), &nc_off, (uint8_t*)nonce.c_str(), stream_block,
        (uint8_t*)packet.payload.data(), (uint8_t*)&encrypted[nonce.size()]) != 0) {
        ESP_LOGE(TAG, "Failed to encrypt audio data");
        return false;
    }

    if (udp_->Send(encrypted) <= 0) {
        ++audio_send_failures_;
        ++audio_send_consecutive_failures_;
        const uint32_t session_age_ms = audio_channel_open_us_ > 0 && now_us >= audio_channel_open_us_
            ? static_cast<uint32_t>((now_us - audio_channel_open_us_) / 1000)
            : 0;
        const uint32_t since_last_attempt_ms = last_audio_attempt_us_ > 0 && now_us >= last_audio_attempt_us_
            ? static_cast<uint32_t>((now_us - last_audio_attempt_us_) / 1000)
            : 0;
        const uint32_t since_last_success_ms = last_audio_success_us_ > 0 && now_us >= last_audio_success_us_
            ? static_cast<uint32_t>((now_us - last_audio_success_us_) / 1000)
            : 0;
        const unsigned long ok_bytes_log = audio_send_success_bytes_ > 0xFFFFFFFFULL
            ? 0xFFFFFFFFUL
            : static_cast<unsigned long>(audio_send_success_bytes_);
        ESP_LOGW(TAG,
            "UDP audio send failed: seq=%lu ts=%lu payload=%u encrypted=%u session=%s session_age_ms=%u attempts=%lu ok=%lu fail=%lu fail_streak=%lu ok_bytes=%lu since_last_attempt_ms=%u since_last_success_ms=%u",
            static_cast<unsigned long>(local_sequence_),
            static_cast<unsigned long>(packet.timestamp),
            static_cast<unsigned>(packet.payload.size()),
            static_cast<unsigned>(encrypted.size()),
            session_id_.c_str(),
            static_cast<unsigned>(session_age_ms),
            static_cast<unsigned long>(audio_send_attempts_),
            static_cast<unsigned long>(audio_send_successes_),
            static_cast<unsigned long>(audio_send_failures_),
            static_cast<unsigned long>(audio_send_consecutive_failures_),
            ok_bytes_log,
            static_cast<unsigned>(since_last_attempt_ms),
            static_cast<unsigned>(since_last_success_ms));
        last_audio_attempt_us_ = now_us;
        return false;
    }
    ++audio_send_successes_;
    audio_send_consecutive_failures_ = 0;
    audio_send_success_bytes_ += packet.payload.size();
    last_audio_attempt_us_ = now_us;
    last_audio_success_us_ = now_us;
    return true;
}

void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    ESP_LOGI(TAG, "Closing audio channel, send_goodbye: %d", send_goodbye);

    // Only send goodbye when client initiates the close
    // Don't send if server already sent goodbye (to avoid ping-pong)
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    ResetAudioTransport(true);
}

bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "MQTT is not connected, try to connect now");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        ResetMqttClient("failed to publish hello message", false);
        ScheduleReconnect(MQTT_FAST_RECONNECT_INTERVAL_MS, "failed to publish hello message");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        ResetMqttClient("server hello timeout", false);
        ScheduleReconnect(MQTT_FAST_RECONNECT_INTERVAL_MS, "server hello timeout");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP Encrypted OPUS Packet Format:
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "Invalid audio packet size: %u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "Invalid audio packet type: %x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        const uint32_t now_ms = static_cast<uint32_t>(esp_log_timestamp());
        if (sequence < remote_sequence_) {
            if (last_remote_sequence_warning_ms_ == 0 || now_ms - last_remote_sequence_warning_ms_ >= 10000) {
                last_remote_sequence_warning_ms_ = now_ms;
                ESP_LOGW(TAG, "Received audio packet with old sequence: %lu, expected: %lu", sequence, remote_sequence_);
            }
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            const uint32_t gap = sequence - (remote_sequence_ + 1);
            if (gap >= 3 && (last_remote_sequence_warning_ms_ == 0 || now_ms - last_remote_sequence_warning_ms_ >= 10000)) {
                last_remote_sequence_warning_ms_ = now_ms;
                ESP_LOGW(TAG,
                         "Received audio packet with wrong sequence: %lu, expected: %lu gap=%lu",
                         sequence,
                         remote_sequence_ + 1,
                         static_cast<unsigned long>(gap));
            }
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        size_t nc_off = 0;
        uint8_t stream_block[16] = {0};
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        int ret = mbedtls_aes_crypt_ctr(&aes_ctx_, decrypted_size, &nc_off, nonce, stream_block, encrypted, (uint8_t*)packet->payload.data());
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to decrypt audio data, ret: %d", ret);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    udp_->Connect(udp_server_, udp_port_);
    audio_send_attempts_ = 0;
    audio_send_successes_ = 0;
    audio_send_failures_ = 0;
    audio_send_consecutive_failures_ = 0;
    audio_send_success_bytes_ = 0;
    audio_channel_open_us_ = esp_timer_get_time();
    last_audio_attempt_us_ = 0;
    last_audio_success_us_ = 0;
    last_remote_sequence_warning_ms_ = 0;

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

std::string MqttProtocol::GetHelloMessage() {
    // 发送 hello 消息申请 UDP 通道
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void MqttProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "udp") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // Get sample rate from hello message
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "UDP is not specified");
        return;
    }
    udp_server_ = cJSON_GetObjectItem(udp, "server")->valuestring;
    udp_port_ = cJSON_GetObjectItem(udp, "port")->valueint;
    auto key = cJSON_GetObjectItem(udp, "key")->valuestring;
    auto nonce = cJSON_GetObjectItem(udp, "nonce")->valuestring;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    aes_nonce_ = DecodeHexString(nonce);
    mbedtls_aes_init(&aes_ctx_);
    mbedtls_aes_setkey_enc(&aes_ctx_, (const unsigned char*)DecodeHexString(key).c_str(), 128);
    local_sequence_ = 0;
    remote_sequence_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
