#include "application_runtime_controller.h"

#include <esp_log.h>
#include <esp_system.h>

#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "ota.h"

namespace {
constexpr char kTag[] = "AppRuntime";
}

void ApplicationRuntimeController::Reboot(std::unique_ptr<Protocol>& protocol,
                                          AudioService& audio_service) {
    ESP_LOGI(kTag, "Rebooting...");
    if (protocol && protocol->IsAudioChannelOpened()) {
        protocol->CloseAudioChannel();
    }
    protocol.reset();
    audio_service.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool ApplicationRuntimeController::UpgradeFirmware(
    std::unique_ptr<Protocol>& protocol,
    AudioService& audio_service,
    const std::string& url,
    const std::string& version,
    const std::function<void(const char*, const char*, const char*, const std::string_view&)>& alert,
    const std::function<void(DeviceState)>& set_device_state,
    const std::function<void()>& reboot,
    const std::function<void(std::function<void()>&&)>& schedule) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (protocol && protocol->IsAudioChannelOpened()) {
        ESP_LOGI(kTag, "Closing audio channel before firmware upgrade");
        protocol->CloseAudioChannel();
    }
    ESP_LOGI(kTag, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    set_device_state(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [schedule, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, static_cast<unsigned>(speed / 1024));
        schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        ESP_LOGE(kTag, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    ESP_LOGI(kTag, "Firmware upgrade successful, rebooting...");
    display->SetChatMessage("system", "Upgrade successful, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    reboot();
    return true;
}

bool ApplicationRuntimeController::CanEnterSleepMode(DeviceState state,
                                                     Protocol* protocol,
                                                     AudioService& audio_service) const {
    if (state != kDeviceStateIdle) {
        return false;
    }

    if (protocol && protocol->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service.IsIdle()) {
        return false;
    }

    return true;
}

void ApplicationRuntimeController::SendMcpMessage(
    const std::function<Protocol*()>& get_protocol,
    const std::function<void(std::function<void()>&&)>& schedule,
    std::string payload) {
    schedule([get_protocol, payload = std::move(payload)]() {
        Protocol* protocol = get_protocol();
        if (protocol) {
            protocol->SendMcpMessage(payload);
        }
    });
}

void ApplicationRuntimeController::ResetProtocol(
    std::unique_ptr<Protocol>& protocol,
    const std::function<void(std::function<void()>&&)>& schedule) {
    auto* protocol_holder = &protocol;
    schedule([protocol_holder]() {
        if (*protocol_holder && (*protocol_holder)->IsAudioChannelOpened()) {
            (*protocol_holder)->CloseAudioChannel();
        }
        protocol_holder->reset();
    });
}
