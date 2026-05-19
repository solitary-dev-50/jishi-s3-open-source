#include "application_activation_controller.h"

#include <algorithm>
#include <array>

#include <esp_log.h>

#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "AppActivation";

}  // namespace

void ApplicationActivationController::RunActivationTask(
    std::unique_ptr<Ota>& ota,
    bool& assets_version_checked,
    EventGroupHandle_t event_group,
    EventBits_t completion_bit,
    AudioService& audio_service,
    const Callbacks& callbacks,
    const std::function<void()>& initialize_protocol) {
    ota = std::make_unique<Ota>();

    CheckAssetsVersion(assets_version_checked, callbacks);
    CheckNewVersion(*ota, audio_service, callbacks);
    initialize_protocol();

    xEventGroupSetBits(event_group, completion_bit);
}

void ApplicationActivationController::CheckAssetsVersion(bool& assets_version_checked,
                                                         const Callbacks& callbacks) {
    if (assets_version_checked) {
        return;
    }
    assets_version_checked = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        callbacks.alert(Lang::Strings::LOADING_ASSETS,
                        message,
                        "cloud_arrow_down",
                        Lang::Sounds::OGG_UPGRADE);

        vTaskDelay(pdMS_TO_TICKS(3000));
        callbacks.set_device_state(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, static_cast<unsigned>(speed / 1024));
            display->SetChatMessage("system", buffer);
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            callbacks.alert(Lang::Strings::ERROR,
                            Lang::Strings::DOWNLOAD_ASSETS_FAILED,
                            "circle_xmark",
                            Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            callbacks.set_device_state(kDeviceStateActivating);
            return;
        }
    }

    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void ApplicationActivationController::CheckNewVersion(Ota& ota,
                                                      AudioService& audio_service,
                                                      const Callbacks& callbacks) {
    const int kMaxRetry = 10;
    int retry_count = 0;
    int retry_delay = 10;

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= kMaxRetry) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            callbacks.alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG,
                     "Check new version failed, retry in %d seconds (%d/%d)",
                     retry_delay,
                     retry_count,
                     kMaxRetry);
            for (int i = 0; i < retry_delay; ++i) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (callbacks.get_device_state() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;
            continue;
        }

        retry_count = 0;
        retry_delay = 10;

        if (ota.HasNewVersion()) {
            if (callbacks.upgrade_firmware(ota.GetFirmwareUrl(), ota.GetFirmwareVersion())) {
                return;
            }
        }

        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        if (ota.HasActivationCode()) {
            ShowActivationCode(audio_service, ota.GetActivationCode(), ota.GetActivationMessage());
        }

        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t activate_err = ota.Activate();
            if (activate_err == ESP_OK) {
                break;
            } else if (activate_err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (callbacks.get_device_state() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void ApplicationActivationController::ShowActivationCode(AudioService& audio_service,
                                                         const std::string& code,
                                                         const std::string& message) {
    struct DigitSound {
        char digit;
        const std::string_view& sound;
    };

    static const std::array<DigitSound, 10> kDigitSounds{{
        {'0', Lang::Sounds::OGG_0},
        {'1', Lang::Sounds::OGG_1},
        {'2', Lang::Sounds::OGG_2},
        {'3', Lang::Sounds::OGG_3},
        {'4', Lang::Sounds::OGG_4},
        {'5', Lang::Sounds::OGG_5},
        {'6', Lang::Sounds::OGG_6},
        {'7', Lang::Sounds::OGG_7},
        {'8', Lang::Sounds::OGG_8},
        {'9', Lang::Sounds::OGG_9},
    }};

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::ACTIVATION);
    display->SetEmotion("link");
    display->SetChatMessage("system", message.c_str());
    audio_service.PlaySound(Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(kDigitSounds.begin(), kDigitSounds.end(),
                               [digit](const DigitSound& item) { return item.digit == digit; });
        if (it != kDigitSounds.end()) {
            audio_service.PlaySound(it->sound);
        }
    }
}
