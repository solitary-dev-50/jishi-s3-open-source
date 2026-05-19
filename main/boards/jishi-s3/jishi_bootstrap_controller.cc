#include "jishi_bootstrap_controller.h"

#include "config.h"
#include "display/display.h"
#include "display/oled_display.h"

#include <driver/gpio.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/task.h>

namespace {
constexpr const char* TAG = "MossBootstrap";
constexpr bool kEnablePanSelfTestOnBoot = true;
constexpr uint32_t kPanSelfTestMoveDurationMs = 700;
constexpr uint32_t kPanSelfTestStepDelayMs = 900;
}

MossBootstrapController::MossBootstrapController(i2c_master_bus_handle_t& i2c_bus,
                                                 Display*& display,
                                                 Tlv320SimplexAudioCodec*& audio_codec,                                                 MossServoController& servo_controller,
                                                 MossIdleMotionController& idle_motion_controller,
                                                 esp_lcd_panel_io_handle_t& panel_io,
                                                 esp_lcd_panel_handle_t& panel)
    : i2c_bus_(i2c_bus),
      display_(display),
      audio_codec_(audio_codec),      servo_controller_(servo_controller),
      idle_motion_controller_(idle_motion_controller),
      panel_io_(panel_io),
      panel_(panel) {}

void MossBootstrapController::LogSharedI2cLevels(const char* stage) const {
    ESP_LOGW(TAG,
             "Shared I2C levels [%s]: SDA=%d SCL=%d CAM_LDO=%d CAM_PWDN=%d CAM_RST=%d",
             stage,
             gpio_get_level(AUDIO_CODEC_I2C_SDA_PIN),
             gpio_get_level(AUDIO_CODEC_I2C_SCL_PIN),
             gpio_get_level(CAMERA_PIN_LDO_EN),
             gpio_get_level(CAMERA_PIN_PWDN),
             gpio_get_level(CAMERA_PIN_RESET));
}

void MossBootstrapController::InitializeI2c() {
    ESP_LOGW(TAG, "InitializeI2c: begin port=%d sda=%d scl=%d",
             static_cast<int>(CAMERA_SCCB_I2C_PORT),
             static_cast<int>(AUDIO_CODEC_I2C_SDA_PIN),
             static_cast<int>(AUDIO_CODEC_I2C_SCL_PIN));
    if (i2c_bus_ != nullptr) {
        ESP_ERROR_CHECK(i2c_del_master_bus(i2c_bus_));
        i2c_bus_ = nullptr;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CAMERA_SCCB_I2C_PORT,
        .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
        .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_));
    LogSharedI2cLevels("after i2c master init");
    ESP_LOGW(TAG, "InitializeI2c: complete");
}

void MossBootstrapController::ForceCameraOffForSharedI2cBoot() const {
    ESP_LOGW(TAG, "ForceCameraOffForSharedI2cBoot: begin");
    gpio_config_t camera_gpio_cfg = {
        .pin_bit_mask = (1ULL << CAMERA_PIN_LDO_EN) |
                        (1ULL << CAMERA_PIN_PWDN) |
                        (1ULL << CAMERA_PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&camera_gpio_cfg));

    gpio_set_level(CAMERA_PIN_RESET, 0);
    gpio_set_level(CAMERA_PIN_PWDN, 1);
    gpio_set_level(CAMERA_PIN_LDO_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    LogSharedI2cLevels("after camera forced off");
}

void MossBootstrapController::DestroyDisplayPanel() {
    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
        panel_io_ = nullptr;
    }
}

void MossBootstrapController::RecoverSharedI2cBus(const char* reason) {
    ESP_LOGW(TAG, "Recover shared I2C bus: %s", reason);
    DestroyDisplayPanel();
    if (i2c_bus_ != nullptr) {
        ESP_ERROR_CHECK(i2c_del_master_bus(i2c_bus_));
        i2c_bus_ = nullptr;
    }

    ForceCameraOffForSharedI2cBoot();

    gpio_config_t scl_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SCL_PIN),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t sda_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SDA_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&scl_cfg));
    ESP_ERROR_CHECK(gpio_config(&sda_cfg));
    LogSharedI2cLevels("before recovery pulses");
    gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9; ++i) {
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 0);
        esp_rom_delay_us(10);
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
        esp_rom_delay_us(10);
    }
    LogSharedI2cLevels("after recovery pulses");

    InitializeI2c();
}

void MossBootstrapController::PrimeSharedI2cBusAtBoot() const {
    ESP_LOGW(TAG, "Prime shared I2C bus before first init");

    ForceCameraOffForSharedI2cBoot();

    gpio_config_t scl_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SCL_PIN),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config_t sda_cfg = {
        .pin_bit_mask = (1ULL << AUDIO_CODEC_I2C_SDA_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&scl_cfg));
    ESP_ERROR_CHECK(gpio_config(&sda_cfg));
    LogSharedI2cLevels("before boot prime pulses");
    gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9; ++i) {
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 0);
        esp_rom_delay_us(10);
        gpio_set_level(AUDIO_CODEC_I2C_SCL_PIN, 1);
        esp_rom_delay_us(10);
    }
    LogSharedI2cLevels("after boot prime pulses");
}

bool MossBootstrapController::InitializeDisplay() {
    ESP_LOGW(TAG, "InitializeDisplay: begin");
    DestroyDisplayPanel();
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = DISPLAY_I2C_ADDRESS,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 0,
        },
        .scl_speed_hz = 400 * 1000,
    };

    if (esp_lcd_new_panel_io_i2c_v2(i2c_bus_, &io_config, &panel_io_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create OLED panel IO");
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel IO created");

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = -1;
    panel_config.bits_per_pixel = 1;

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
    };
    panel_config.vendor_config = &ssd1306_config;

    if (esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SSD1306 panel");
        DestroyDisplayPanel();
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel created");
    if (esp_lcd_panel_reset(panel_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset OLED panel");
        DestroyDisplayPanel();
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel reset complete");
    if (esp_lcd_panel_init(panel_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize OLED panel");
        DestroyDisplayPanel();
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel init complete");
    if (esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply OLED mirror configuration");
        DestroyDisplayPanel();
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel mirror complete");

    if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power on OLED panel");
        DestroyDisplayPanel();
        return false;
    }
    ESP_LOGW(TAG, "InitializeDisplay: panel power on complete");
    display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    ESP_LOGW(TAG, "InitializeDisplay: complete");
    return true;
}

void MossBootstrapController::InitializeAudioCodec() {
    audio_codec_ = new Tlv320SimplexAudioCodec(i2c_bus_);
}

void MossBootstrapController::InitializeServos() {
    servo_controller_.Initialize();
    idle_motion_controller_.Initialize();
}

void MossBootstrapController::RunPanSelfTest() const {
    if (!kEnablePanSelfTestOnBoot) {
        return;
    }

    ESP_LOGW(TAG, "PAN self-test start: 90 -> 60 -> 120 -> 90");
    servo_controller_.SetManualPose(SERVO_IDLE_TILT_ANGLE, 90.0f, kPanSelfTestMoveDurationMs);
    vTaskDelay(pdMS_TO_TICKS(kPanSelfTestStepDelayMs));
    servo_controller_.SetManualPose(SERVO_IDLE_TILT_ANGLE, 60.0f, kPanSelfTestMoveDurationMs);
    vTaskDelay(pdMS_TO_TICKS(kPanSelfTestStepDelayMs));
    servo_controller_.SetManualPose(SERVO_IDLE_TILT_ANGLE, 120.0f, kPanSelfTestMoveDurationMs);
    vTaskDelay(pdMS_TO_TICKS(kPanSelfTestStepDelayMs));
    servo_controller_.SetManualPose(SERVO_IDLE_TILT_ANGLE, 90.0f, kPanSelfTestMoveDurationMs);
    vTaskDelay(pdMS_TO_TICKS(kPanSelfTestStepDelayMs));
    ESP_LOGW(TAG, "PAN self-test complete");
}


