#ifndef _JISHI_BOOTSTRAP_CONTROLLER_H_
#define _JISHI_BOOTSTRAP_CONTROLLER_H_

#include "esp32_camera.h"
#include "jishi_idle_motion_controller.h"
#include "jishi_servo_controller.h"
#include "tlv320_simplex_audio_codec.h"

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

class Display;

class MossBootstrapController {
public:
    MossBootstrapController(i2c_master_bus_handle_t& i2c_bus,
                            Display*& display,
                            Tlv320SimplexAudioCodec*& audio_codec,                            MossServoController& servo_controller,
                            MossIdleMotionController& idle_motion_controller,
                            esp_lcd_panel_io_handle_t& panel_io,
                            esp_lcd_panel_handle_t& panel);

    void PrimeSharedI2cBusAtBoot() const;
    void InitializeI2c();
    void ForceCameraOffForSharedI2cBoot() const;
    void RecoverSharedI2cBus(const char* reason);
    bool InitializeDisplay();
    void InitializeAudioCodec();
    void InitializeServos();
    void RunPanSelfTest() const;

private:
    void LogSharedI2cLevels(const char* stage) const;
    void DestroyDisplayPanel();

    i2c_master_bus_handle_t& i2c_bus_;
    Display*& display_;
    Tlv320SimplexAudioCodec*& audio_codec_;    MossServoController& servo_controller_;
    MossIdleMotionController& idle_motion_controller_;
    esp_lcd_panel_io_handle_t& panel_io_;
    esp_lcd_panel_handle_t& panel_;
};

#endif // _JISHI_BOOTSTRAP_CONTROLLER_H_


