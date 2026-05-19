#include "mcp_system_tools.h"

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "application.h"
#include "assets.h"
#include "board.h"
#include "display.h"
#include "lvgl_display.h"
#include "lvgl_theme.h"
#include "mcp_camera_tools.h"
#include "mcp_server.h"
#include "oled_display.h"
#include "settings.h"

namespace {
constexpr char kTag[] = "McpSystemTools";
constexpr const char* kWebsocketSettingsNs = "websocket";
constexpr const char* kServerProfilesNs = "server_profiles";
constexpr const char* kOfficialProfile = "official";
constexpr const char* kSelfHostedProfile = "self_hosted";
constexpr int kDefaultProtocolVersion = 3;
constexpr const char* kDefaultSelfHostedUrl = "";
constexpr const char* kDefaultSelfHostedOtaUrl = "";

std::string TrimAsciiWhitespace(const std::string& input) {
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

bool IsOfficialServiceUrl(const std::string& url) {
    return url.find("xiaozhi.me") != std::string::npos;
}

std::string DetectConnectionProfile(const std::string& url) {
    return IsOfficialServiceUrl(url) ? kOfficialProfile : kSelfHostedProfile;
}

std::string RedactTokenPreview(const std::string& token) {
    if (token.empty()) {
        return "";
    }
    if (token.size() <= 8) {
        return "****";
    }
    return token.substr(0, 4) + "..." + token.substr(token.size() - 4);
}

bool SaveConnectionProfileSnapshot(const std::string& profile_name,
                                   const std::string& url,
                                   const std::string& token,
                                   int version) {
    const bool is_official = profile_name == kOfficialProfile;
    const std::string key_prefix = is_official ? "off" : "self";
    Settings settings(kServerProfilesNs, true);
    return settings.SetString(key_prefix + "_url", url) &&
           settings.SetString(key_prefix + "_tok", token) &&
           settings.SetInt(key_prefix + "_ver", version);
}

bool LoadConnectionProfileSnapshot(const std::string& profile_name,
                                   std::string* url,
                                   std::string* token,
                                   int* version) {
    const bool is_official = profile_name == kOfficialProfile;
    const std::string key_prefix = is_official ? "off" : "self";
    Settings settings(kServerProfilesNs, false);
    *url = settings.GetString(key_prefix + "_url");
    *token = settings.GetString(key_prefix + "_tok");
    *version = settings.GetInt(key_prefix + "_ver", 0);
    return !url->empty();
}

bool SaveConnectionProfileOtaUrl(const std::string& profile_name, const std::string& ota_url) {
    const bool is_official = profile_name == kOfficialProfile;
    const std::string key_prefix = is_official ? "off" : "self";
    Settings settings(kServerProfilesNs, true);
    if (ota_url.empty()) {
        settings.EraseKey(key_prefix + "_ota");
        return true;
    }
    return settings.SetString(key_prefix + "_ota", ota_url);
}

std::string LoadConnectionProfileOtaUrl(const std::string& profile_name) {
    const bool is_official = profile_name == kOfficialProfile;
    const std::string key_prefix = is_official ? "off" : "self";
    Settings settings(kServerProfilesNs, false);
    return settings.GetString(key_prefix + "_ota");
}

cJSON* BuildConnectionProfileJson(const std::string& active_profile,
                                  const std::string& current_url,
                                  const std::string& current_token,
                                  int current_version) {
    std::string official_url;
    std::string official_token;
    int official_version = 0;
    std::string self_hosted_url;
    std::string self_hosted_token;
    int self_hosted_version = 0;
    const bool has_official = LoadConnectionProfileSnapshot(kOfficialProfile, &official_url, &official_token, &official_version);
    const bool has_self_hosted = LoadConnectionProfileSnapshot(kSelfHostedProfile, &self_hosted_url, &self_hosted_token, &self_hosted_version);

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "active_profile", active_profile.c_str());
    cJSON_AddStringToObject(json, "current_url", current_url.c_str());
    cJSON_AddBoolToObject(json, "current_has_token", !current_token.empty());
    cJSON_AddStringToObject(json, "current_token_preview", RedactTokenPreview(current_token).c_str());
    cJSON_AddNumberToObject(json, "current_version", current_version);

    cJSON* profiles = cJSON_CreateObject();

    cJSON* official = cJSON_CreateObject();
    cJSON_AddBoolToObject(official, "saved", has_official);
    if (has_official) {
        cJSON_AddStringToObject(official, "url", official_url.c_str());
        cJSON_AddBoolToObject(official, "has_token", !official_token.empty());
        cJSON_AddStringToObject(official, "token_preview", RedactTokenPreview(official_token).c_str());
        cJSON_AddNumberToObject(official, "version", official_version);
    }
    cJSON_AddItemToObject(profiles, kOfficialProfile, official);

    cJSON* self_hosted = cJSON_CreateObject();
    cJSON_AddBoolToObject(self_hosted, "saved", has_self_hosted);
    if (has_self_hosted) {
        cJSON_AddStringToObject(self_hosted, "url", self_hosted_url.c_str());
        cJSON_AddBoolToObject(self_hosted, "has_token", !self_hosted_token.empty());
        cJSON_AddStringToObject(self_hosted, "token_preview", RedactTokenPreview(self_hosted_token).c_str());
        cJSON_AddNumberToObject(self_hosted, "version", self_hosted_version);
    }
    cJSON_AddItemToObject(profiles, kSelfHostedProfile, self_hosted);

    cJSON_AddItemToObject(json, "profiles", profiles);
    return json;
}

cJSON* SwitchConnectionProfileInternal(const std::string& requested_profile,
                                       const std::string& provided_url,
                                       const std::string& provided_token,
                                       int provided_version,
                                       bool force_reconnect_if_unchanged = false) {
    if (requested_profile != kOfficialProfile && requested_profile != kSelfHostedProfile) {
        throw std::runtime_error("profile must be `official` or `self_hosted`");
    }

    Settings websocket_settings(kWebsocketSettingsNs, true);
    const std::string current_url = websocket_settings.GetString("url");
    const std::string current_token = websocket_settings.GetString("token");
    const int current_version = websocket_settings.GetInt("version", kDefaultProtocolVersion);
    Settings wifi_settings("wifi", true);
    const std::string current_ota_url = wifi_settings.GetString("ota_url");

    Settings profile_settings(kServerProfilesNs, true);
    std::string current_profile = profile_settings.GetString("active_profile");
    if (current_profile.empty()) {
        current_profile = DetectConnectionProfile(current_url);
    }

    if (!SaveConnectionProfileSnapshot(current_profile, current_url, current_token, current_version) ||
        !SaveConnectionProfileOtaUrl(current_profile, current_ota_url)) {
        throw std::runtime_error("Failed to snapshot current connection profile");
    }

    std::string target_url;
    std::string target_token;
    int target_version = 0;
    std::string target_ota_url;

    if (requested_profile == kOfficialProfile) {
        if (current_profile == kOfficialProfile && !current_url.empty()) {
            target_url = current_url;
            target_token = current_token;
            target_version = current_version;
            target_ota_url = current_ota_url;
        } else if (LoadConnectionProfileSnapshot(kOfficialProfile, &target_url, &target_token, &target_version)) {
            target_ota_url = LoadConnectionProfileOtaUrl(kOfficialProfile);
        } else {
            throw std::runtime_error("No saved official connection profile");
        }
    } else {
        if (!provided_url.empty()) {
            target_url = provided_url;
            target_token = provided_token;
            target_version = provided_version > 0 ? provided_version : current_version;
            target_ota_url = kDefaultSelfHostedOtaUrl;
        } else if (current_profile == kSelfHostedProfile && !current_url.empty()) {
            target_url = current_url;
            target_token = current_token;
            target_version = current_version;
            target_ota_url = current_ota_url.empty() ? kDefaultSelfHostedOtaUrl : current_ota_url;
        } else if (LoadConnectionProfileSnapshot(kSelfHostedProfile, &target_url, &target_token, &target_version)) {
            target_ota_url = LoadConnectionProfileOtaUrl(kSelfHostedProfile);
            if (target_ota_url.empty()) {
                target_ota_url = kDefaultSelfHostedOtaUrl;
            }
        } else {
            target_url = kDefaultSelfHostedUrl;
            target_token = "";
            target_version = current_version;
            target_ota_url = kDefaultSelfHostedOtaUrl;
        }
    }

    if (target_url.empty()) {
        throw std::runtime_error("Target connection profile has no websocket URL");
    }
    if (target_version <= 0) {
        target_version = current_version > 0 ? current_version : kDefaultProtocolVersion;
    }

    const bool unchanged = current_profile == requested_profile &&
                           current_url == target_url &&
                           current_token == target_token &&
                           current_version == target_version &&
                           current_ota_url == target_ota_url;
    if (unchanged && !force_reconnect_if_unchanged) {
        cJSON* json = BuildConnectionProfileJson(requested_profile, current_url, current_token, current_version);
        cJSON_AddBoolToObject(json, "switched", false);
        cJSON_AddStringToObject(json, "message", "Already using the requested connection profile");
        return json;
    }

    websocket_settings.SetString("url", target_url);
    websocket_settings.SetString("token", target_token);
    websocket_settings.SetInt("version", target_version);
    wifi_settings.SetString("ota_url", target_ota_url);
    profile_settings.SetString("active_profile", requested_profile);

    SaveConnectionProfileSnapshot(requested_profile, target_url, target_token, target_version);
    SaveConnectionProfileOtaUrl(requested_profile, target_ota_url);

    cJSON* json = BuildConnectionProfileJson(requested_profile, target_url, target_token, target_version);
    cJSON_AddBoolToObject(json, "switched", true);
    cJSON_AddStringToObject(json, "requested_profile", requested_profile.c_str());
    cJSON_AddStringToObject(json, "message", "Connection profile switched; device will reboot");

    auto& app = Application::GetInstance();
    app.Schedule([&app]() {
        ESP_LOGW(kTag, "User requested backend connection profile switch, rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        app.Reboot();
    });

    return json;
}
}  // namespace

void McpSystemToolsRegistrar::AddCommonTools(McpServer& server) {
    auto& board = Board::GetInstance();

    server.AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList&) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    server.AddTool("self.audio_speaker.set_volume",
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }),
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });

    auto backlight = board.GetBacklight();
    if (backlight) {
        server.AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        server.AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    McpCameraToolsRegistrar::AddCommonTools(server);
#endif
}

void McpSystemToolsRegistrar::AddUserOnlyTools(McpServer& server) {
    server.AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    server.AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(kTag, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));
                app.Reboot();
            });
            return true;
        });

    server.AddTool("self.system.get_connection_profile",
        "Get the current backend connection profile used by the device.\n"
        "Use this tool when the user asks whether the device is connected to the official xiaozhi.me service or a self-hosted jishi-ai-system service.\n"
        "Return the current websocket endpoint, whether a token is configured, and whether saved official/self_hosted profiles exist.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            Settings websocket_settings(kWebsocketSettingsNs, false);
            const std::string current_url = websocket_settings.GetString("url");
            const std::string current_token = websocket_settings.GetString("token");
            const int current_version = websocket_settings.GetInt("version", kDefaultProtocolVersion);

            Settings profile_settings(kServerProfilesNs, false);
            std::string active_profile = profile_settings.GetString("active_profile");
            if (active_profile.empty()) {
                active_profile = DetectConnectionProfile(current_url);
            }
            return BuildConnectionProfileJson(active_profile, current_url, current_token, current_version);
        });


    server.AddTool("self.system.switch_connection_profile",
        "Switch the device backend connection between the official xiaozhi.me service and a self-hosted jishi-ai-system service.\n"
        "Use this tool only when the user explicitly asks to switch the backend service.\n"
        "Args:\n"
        "  `profile`: `official` or `self_hosted`.\n"
        "  `url`: optional websocket URL for the self_hosted profile. Use this on first setup or when the self-hosted address changes.\n"
        "  `token`: optional token for the target profile. Leave empty if the target server does not require a token.\n"
        "  `version`: optional websocket protocol version. Use 0 to keep the saved/current version.\n"
        "Behavior:\n"
        "  - The current profile is snapshotted before switching.\n"
        "  - The target profile must already be saved unless you provide `url` for `self_hosted`.\n"
        "  - The device will reboot after a successful switch.",
        PropertyList({
            Property("profile", kPropertyTypeString),
            Property("url", kPropertyTypeString, std::string("")),
            Property("token", kPropertyTypeString, std::string("")),
            Property("version", kPropertyTypeInteger, 0)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string requested_profile = TrimAsciiWhitespace(properties["profile"].value<std::string>());
            const std::string provided_url = TrimAsciiWhitespace(properties["url"].value<std::string>());
            const std::string provided_token = properties["token"].value<std::string>();
            const int provided_version = properties["version"].value<int>();
            return SwitchConnectionProfileInternal(requested_profile, provided_url, provided_token, provided_version);
        });

    server.AddTool("self.system.switch_to_local_service",
        "Switch the device backend to the local self-hosted jishi-ai-system service.\n"
        "Use this tool when the user says phrases like: `切到本地服务`, `切换到本地服务`, `使用本地服务`.\n"
        "Behavior:\n"
        "  - Prefer the saved self_hosted profile.\n"
        "  - If no self_hosted profile is saved yet, use the built-in local websocket address.\n"
        "  - The device will reboot after a successful switch.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return SwitchConnectionProfileInternal(kSelfHostedProfile, "", "", 0, true);
        });

    server.AddTool("self.system.switch_to_official_service",
        "Switch the device backend back to the official xiaozhi.me cloud service.\n"
        "Use this tool when the user says phrases like: `切换云端服务`, `切到云端服务`, `使用云端服务`.\n"
        "Behavior:\n"
        "  - Use the saved official profile.\n"
        "  - The device will reboot after a successful switch.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return SwitchConnectionProfileInternal(kOfficialProfile, "", "", 0, true);
        });

    server.AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(kTag, "User requested firmware upgrade from URL: %s", url.c_str());

            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(kTag, "Firmware upgrade failed");
                }
            });

            return true;
        });

#ifdef HAVE_LVGL
    auto& board = Board::GetInstance();
    auto display = dynamic_cast<LvglDisplay*>(board.GetDisplay());
    McpCameraToolsRegistrar::AddUserOnlyTools(server);
    if (display) {
        server.AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList&) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                cJSON_AddBoolToObject(json, "monochrome", dynamic_cast<OledDisplay*>(display) != nullptr);
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        server.AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(kTag, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(kTag, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        server.AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif
    }
#endif

    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        server.AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }

}

