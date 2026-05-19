#include "wifi_board.h"
#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/display.h"
#include "display/oled_display.h"
#include "esp32_camera.h"
#include "jishi_bootstrap_controller.h"
#include "jishi_camera_capture_controller.h"
#include "jishi_camera_hardware_controller.h"
#include "jishi_camera_preview_controller.h"
#include "jishi_camera_session_controller.h"
#include "jishi_led_strip.h"
#include "jishi_idle_motion.h"
#include "jishi_idle_motion_controller.h"
#include "jishi_pan_calibration_controller.h"
#include "jishi_servo_controller.h"
#include "jishi_view_pose_controller.h"
#include "mcp_server.h"
#include "settings.h"
#include "tlv320_simplex_audio_codec.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <driver/i2c_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <cJSON.h>

#define TAG "MossS3Board"

class MossS3Board : public WifiBoard {
private:
    static constexpr uint64_t kBoardPollPeriodUs = 40000ULL;
    static constexpr uint32_t kBatteryPolicyRefreshMs = 5000;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Display* display_ = nullptr;    MossIdleMotion idle_motion_;
    Tlv320SimplexAudioCodec* audio_codec_ = nullptr;
    AdcBatteryMonitor* battery_monitor_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    MossServoController servo_controller_;
    Button boot_button_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    esp_timer_handle_t board_timer_ = nullptr;
    DeviceState current_state_ = kDeviceStateUnknown;
    float last_applied_pan_ = SERVO_CENTER_ANGLE;    uint32_t last_battery_policy_refresh_ms_ = 0;
    uint32_t listening_started_ms_ = 0;
    bool idle_motion_suppressed_ = false;    MossViewPoseController view_pose_controller_{servo_controller_};
    MossPanCalibrationController pan_calibration_controller_{servo_controller_};
    MossIdleMotionController idle_motion_controller_{idle_motion_, servo_controller_};
    std::atomic<bool> camera_i2c_occupied_{false};
    MossCameraSessionController camera_session_controller_{camera_i2c_occupied_};
    MossCameraCaptureController capture_controller_;
    MossCameraHardwareController camera_hardware_controller_{camera_, capture_controller_, display_, panel_};
    MossBootstrapController bootstrap_controller_{i2c_bus_,
                                                  display_,
                                                  audio_codec_,                                                  servo_controller_,
                                                  idle_motion_controller_,
                                                  panel_io_,
                                                  panel_};
    MossCameraPreviewController camera_preview_controller_;
    NetworkEventCallback user_network_event_callback_ = nullptr;

    bool IsDocumentCaptureProfile() const {
        return capture_controller_.IsDocumentCaptureProfile();
    }

    void AlignForDocumentCapture() {
        ESP_LOGI(TAG, "Document capture pose: tilt=%.1f duration=%u",
                 view_pose_controller_.GetTiltForPose(CameraViewPose::DocumentDown),
                 static_cast<unsigned>(CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS));
        const float current_pan = servo_controller_.GetCurrentPanAngle();
        servo_controller_.SetManualPose(view_pose_controller_.GetTiltForPose(CameraViewPose::DocumentDown),
                                        current_pan,
                                        CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS);
        vTaskDelay(pdMS_TO_TICKS(CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS +
                                 CAMERA_DOCUMENT_CAPTURE_SETTLE_DELAY_MS));
    }

    static void OnBoardTimer(void* arg) {
        auto* self = static_cast<MossS3Board*>(arg);
        self->RefreshIdleMotionBatteryPolicy(false);
        self->idle_motion_controller_.Poll(self->current_state_, self->idle_motion_suppressed_);
    }

    void RefreshIdleMotionBatteryPolicy(bool force) {
        const uint32_t now_ms = static_cast<uint32_t>(esp_log_timestamp());
        if (!force && (now_ms - last_battery_policy_refresh_ms_) < kBatteryPolicyRefreshMs) {
            return;
        }
        last_battery_policy_refresh_ms_ = now_ms;

        int level = 0;
        bool charging = false;
        bool discharging = false;
        const bool has_battery_info = GetBatteryLevel(level, charging, discharging);
        idle_motion_controller_.UpdateBatteryPolicy(has_battery_info, level, charging);
    }

    void InitializeBoardTimer() {
        esp_timer_create_args_t timer_args = {
            .callback = &MossS3Board::OnBoardTimer,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "jishi_board_poll",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &board_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(board_timer_, kBoardPollPeriodUs));
    }

    static size_t GetInternalSramFree() {
        return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static size_t GetInternalSramLargestBlock() {
        return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    static size_t GetInternalSramMinimum() {
        return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            EnterWifiConfigMode();
        });
    }

    void InitializeAecTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool(
            "self.system.set_aec",
            "设置对话打断模式（AEC）。开启后，说话过程中可以通过插话实时打断；关闭后，仅保留普通唤醒，不进行实时打断。\n"
            "参数：\n"
            "  `enable`: true 为开启，false 为关闭。\n",
            PropertyList({
                Property("enable", kPropertyTypeBoolean)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                const bool enable = properties["enable"].value<bool>();
#if !CONFIG_USE_DEVICE_AEC
                if (enable) {
                    Settings settings("aec", true);
                    settings.SetInt("mode", kAecOff);
                    Application::GetInstance().SetAecMode(kAecOff);
                    return std::string("{\"success\":false,\"enabled\":false,\"message\":\"Ji Shi当前硬件不支持设备端AEC，已保持关闭\"}");
                }
#endif
                Settings settings("aec", true);
                settings.SetInt("mode", enable ? kAecOnDeviceSide : kAecOff);

                auto& app = Application::GetInstance();
                app.StopListening();
                app.SetDeviceState(kDeviceStateIdle);
                app.SetAecMode(enable ? kAecOnDeviceSide : kAecOff);

                return std::string(enable ? "{\"success\":true,\"message\":\"实时打断已开启\"}"
                                          : "{\"success\":true,\"message\":\"实时打断已关闭\"}");
            });

        mcp_server.AddTool(
            "self.system.get_aec",
            "获取当前对话打断模式（AEC）状态。",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                const bool enabled = Application::GetInstance().GetAecMode() != kAecOff;
#if !CONFIG_USE_DEVICE_AEC
                return std::string("{\"success\":true,\"supported\":false,\"enabled\":false,\"message\":\"Ji Shi当前硬件不支持设备端AEC\"}");
#else
                return std::string(enabled ? "{\"success\":true,\"enabled\":true,\"message\":\"实时打断已开启\"}"
                                           : "{\"success\":true,\"enabled\":false,\"message\":\"实时打断已关闭\"}");
#endif
            });
    }

  public:
    MossS3Board()
        : boot_button_(BOOT_BUTTON_GPIO),
          camera_preview_controller_([this](std::string& jpeg_data, std::string* mime_type) {
              return capture_controller_.GetLastCaptureJpeg(jpeg_data, mime_type);
          }) {
        bootstrap_controller_.PrimeSharedI2cBusAtBoot();
        ESP_LOGW(TAG, "Boot path: prime done");
        bootstrap_controller_.InitializeI2c();
        ESP_LOGW(TAG, "Boot path: i2c init done");
        if (!bootstrap_controller_.InitializeDisplay()) {
            ESP_LOGE(TAG, "OLED init failed, continue with NoDisplay");
            display_ = new NoDisplay();
        }
        ESP_LOGW(TAG, "Boot path: display stage done");
        bootstrap_controller_.InitializeAudioCodec();
        ESP_LOGW(TAG, "Boot path: audio codec stage done");
        bootstrap_controller_.InitializeServos();
        bootstrap_controller_.RunPanSelfTest();
        InitializeBoardTimer();
        InitializeButtons();
        auto& app = Application::GetInstance();
        Settings aec_settings("aec", false);
        const int stored_aec_mode = aec_settings.GetInt("mode", kAecOff);
#if CONFIG_USE_DEVICE_AEC
        app.SetAecMode(stored_aec_mode == kAecOff ? kAecOff : kAecOnDeviceSide);
#else
        if (stored_aec_mode != kAecOff) {
            Settings aec_rw("aec", true);
            aec_rw.SetInt("mode", kAecOff);
        }
        app.SetAecMode(kAecOff);
#endif
        InitializeAecTools();
        battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1,
                                                 BATTERY_ADC_CHANNEL,
                                                 BATTERY_ADC_UPPER_RESISTOR_OHM,
                                                 BATTERY_ADC_LOWER_RESISTOR_OHM,
                                                 BATTERY_CHARGING_PIN);
        RefreshIdleMotionBatteryPolicy(true);
        ESP_LOGI(TAG, "JiShi S3 board skeleton ready");
    }

    virtual ~MossS3Board() override {
        camera_preview_controller_.Stop();
        if (board_timer_ != nullptr) {
            esp_timer_stop(board_timer_);
            esp_timer_delete(board_timer_);
            board_timer_ = nullptr;
        }
        delete battery_monitor_;
        delete camera_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        return audio_codec_;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Led* GetLed() override {
        static MossLedStrip led(LED_GPIO, LED_STRIP_MAX_LEDS);
        return &led;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_monitor_ == nullptr) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }
        level = battery_monitor_->GetBatteryLevel();
        charging = battery_monitor_->IsCharging();
        discharging = battery_monitor_->IsDischarging();
        return true;
    }

    virtual bool HasCameraCapability() override {
        return true;
    }

    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        user_network_event_callback_ = callback;
        WifiBoard::SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
            camera_preview_controller_.HandleNetworkEvent(event);
            if (user_network_event_callback_ != nullptr) {
                user_network_event_callback_(event, data);
            }
        });
    }

    virtual void SetNextCaptureProfile(CameraCaptureProfile profile) override {
        capture_controller_.SetNextCaptureProfile(profile);
    }

    virtual std::string GetLastCaptureDebugJson() override {
        return capture_controller_.GetLastCaptureDebugJson();
    }

    virtual bool GetLastCaptureJpeg(std::string& jpeg_data, std::string* mime_type = nullptr) override {
        return capture_controller_.GetLastCaptureJpeg(jpeg_data, mime_type);
    }

    virtual CameraViewPose GetCameraViewPose() const override {
        return view_pose_controller_.GetPose();
    }

    virtual bool SetCameraViewPose(CameraViewPose pose) override {
        return view_pose_controller_.SetCameraViewPose(
            pose,
            current_state_,
            []() {},
            last_applied_pan_,
            idle_motion_suppressed_);
    }

    virtual bool ExecuteServoPanCalibrationAction(ServoPanCalibrationAction action,
                                                  std::string* result = nullptr) override {
        return pan_calibration_controller_.ExecuteAction(
            action,
            current_state_,
            view_pose_controller_,
            []() {},
            last_applied_pan_,
            idle_motion_suppressed_,
            result);
    }

    virtual bool AdjustServoPanCalibration(float delta_deg, std::string* result = nullptr) override {
        return pan_calibration_controller_.Adjust(
            delta_deg,
            view_pose_controller_,
            []() {},
            last_applied_pan_,
            idle_motion_suppressed_,
            result);
    }


    virtual bool IsServoPanCalibrationActive() const override {
        return pan_calibration_controller_.IsActive();
    }

    virtual bool SetServoPanForTest(float pan_angle, uint32_t duration_ms) override {
        const float target_pan = std::clamp(pan_angle,
                                            static_cast<float>(SERVO_PAN_MIN_ANGLE),
                                            static_cast<float>(SERVO_PAN_MAX_ANGLE));
        idle_motion_suppressed_ = true;
        last_applied_pan_ = target_pan;
        const float target_tilt = (view_pose_controller_.GetPose() == CameraViewPose::Default)
                                      ? SERVO_LISTEN_TILT_ANGLE
                                      : view_pose_controller_.GetCurrentTilt();
        ESP_LOGW(TAG,
                 "Servo pan test: tilt=%.1f pan=%.1f duration=%lu state=%d",
                 target_tilt,
                 target_pan,
                 static_cast<unsigned long>(duration_ms),
                 static_cast<int>(current_state_));
        servo_controller_.LockConversationPan(target_pan);
        servo_controller_.SetManualPose(target_tilt, target_pan, duration_ms);
        return true;
    }

    virtual bool CaptureOnce(uint8_t** out_buf, size_t* out_len) override {
        MossCameraSessionController::Callbacks callbacks{
            .is_document_capture_profile = [this]() { return IsDocumentCaptureProfile(); },
            .align_for_document_capture = [this]() { AlignForDocumentCapture(); },
            .set_display_updates_suspended = [this](bool suspended) {
                camera_hardware_controller_.SetDisplayUpdatesSuspended(suspended);
            },
            .force_camera_off_for_shared_i2c_boot = [this]() {
                bootstrap_controller_.ForceCameraOffForSharedI2cBoot();
            },
            .initialize_camera = [this]() { camera_hardware_controller_.InitializeCamera(); },
            .power_down_camera_hardware = [this]() { camera_hardware_controller_.PowerDownCameraHardware(); },
            .recover_display_after_camera_use = [this]() {
                camera_hardware_controller_.RecoverDisplayAfterCameraUse();
            },
            .reset_capture_profile = [this]() { capture_controller_.ResetNextCaptureProfile(); },
            .capture_latest_frame = [this](uint8_t** buf, size_t* len, MossCameraCaptureController::CaptureDebugInfo* debug) {
                return camera_hardware_controller_.CaptureLatestFrameToPsram(buf, len, debug);
            },
            .get_internal_sram_free = []() { return GetInternalSramFree(); },
            .get_internal_sram_largest_block = []() { return GetInternalSramLargestBlock(); },
            .get_internal_sram_minimum = []() { return GetInternalSramMinimum(); },
            .get_psram_free = []() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); },
        };
        return camera_session_controller_.CaptureOnce(camera_, out_buf, out_len, capture_controller_, callbacks);
    }

    virtual void OnDeviceStateChanged(DeviceState state) override {
        current_state_ = state;
        RefreshIdleMotionBatteryPolicy(true);
        if (state == kDeviceStateIdle || state == kDeviceStateActivating ||
            state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
            last_applied_pan_ = SERVO_CENTER_ANGLE;
            listening_started_ms_ = 0;
            idle_motion_suppressed_ = (state != kDeviceStateIdle) || view_pose_controller_.IsActive();
        } else if (state == kDeviceStateListening) {
            listening_started_ms_ = static_cast<uint32_t>(esp_log_timestamp());
            idle_motion_suppressed_ = true;
        } else if (state == kDeviceStateConnecting) {
            listening_started_ms_ = 0;
            idle_motion_suppressed_ = true;
        } else if (state == kDeviceStateSpeaking) {
            listening_started_ms_ = 0;
            idle_motion_suppressed_ = true;
        }
        servo_controller_.OnDeviceStateChanged(state);
        if (pan_calibration_controller_.IsActive()) {
            pan_calibration_controller_.ApplyPose(180,
                                                  view_pose_controller_,
                                                  []() {},
                                                  last_applied_pan_,
                                                  idle_motion_suppressed_);
            return;
        }
        if (view_pose_controller_.IsActive()) {
            view_pose_controller_.ApplyCurrentPose(CAMERA_DOCUMENT_CAPTURE_ALIGN_DURATION_MS);
        }
        if (state == kDeviceStateIdle) {
            idle_motion_suppressed_ = view_pose_controller_.IsActive();
            idle_motion_controller_.Poll(current_state_, idle_motion_suppressed_);
        }
    }

    virtual void OnWakeWordDetected() override {
        idle_motion_suppressed_ = true;
        if (pan_calibration_controller_.IsActive()) {
            return;
        }
        if (view_pose_controller_.IsActive()) {
            return;
        }
        ESP_LOGI(TAG,
                 "Wake callback: state=%d current_pan=%.1f current_tilt=%.1f",
                 static_cast<int>(current_state_),
                 servo_controller_.GetCurrentPanAngle(),
                 servo_controller_.GetCurrentTiltAngle());
        last_applied_pan_ = servo_controller_.GetCurrentPanAngle();
    }
};

DECLARE_BOARD(MossS3Board);







