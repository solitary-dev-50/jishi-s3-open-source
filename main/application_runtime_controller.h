#ifndef APPLICATION_RUNTIME_CONTROLLER_H
#define APPLICATION_RUNTIME_CONTROLLER_H

#include <functional>
#include <memory>
#include <string>

#include "audio_service.h"
#include "device_state.h"
#include "protocol.h"

class ApplicationRuntimeController {
public:
    void Reboot(std::unique_ptr<Protocol>& protocol, AudioService& audio_service);

    bool UpgradeFirmware(std::unique_ptr<Protocol>& protocol,
                         AudioService& audio_service,
                         const std::string& url,
                         const std::string& version,
                         const std::function<void(const char*,
                                                  const char*,
                                                  const char*,
                                                  const std::string_view&)>& alert,
                         const std::function<void(DeviceState)>& set_device_state,
                         const std::function<void()>& reboot,
                         const std::function<void(std::function<void()>&&)>& schedule);

    bool CanEnterSleepMode(DeviceState state,
                           Protocol* protocol,
                           AudioService& audio_service) const;

    void SendMcpMessage(const std::function<Protocol*()>& get_protocol,
                        const std::function<void(std::function<void()>&&)>& schedule,
                        std::string payload);

    void ResetProtocol(std::unique_ptr<Protocol>& protocol,
                       const std::function<void(std::function<void()>&&)>& schedule);
};

#endif  // APPLICATION_RUNTIME_CONTROLLER_H
