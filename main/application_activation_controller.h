#ifndef APPLICATION_ACTIVATION_CONTROLLER_H
#define APPLICATION_ACTIVATION_CONTROLLER_H

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "audio_service.h"
#include "device_state.h"
#include "ota.h"

class ApplicationActivationController {
public:
    struct Callbacks {
        std::function<void(const char* status,
                           const char* message,
                           const char* emotion,
                           const std::string_view& sound)> alert;
        std::function<void(DeviceState state)> set_device_state;
        std::function<DeviceState()> get_device_state;
        std::function<bool(const std::string& url, const std::string& version)> upgrade_firmware;
    };

    void RunActivationTask(std::unique_ptr<Ota>& ota,
                           bool& assets_version_checked,
                           EventGroupHandle_t event_group,
                           EventBits_t completion_bit,
                           AudioService& audio_service,
                           const Callbacks& callbacks,
                           const std::function<void()>& initialize_protocol);

private:
    void CheckAssetsVersion(bool& assets_version_checked, const Callbacks& callbacks);
    void CheckNewVersion(Ota& ota, AudioService& audio_service, const Callbacks& callbacks);
    void ShowActivationCode(AudioService& audio_service,
                            const std::string& code,
                            const std::string& message);
};

#endif  // APPLICATION_ACTIVATION_CONTROLLER_H
